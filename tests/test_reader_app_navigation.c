// Host regression test for the reader's three-button state machine.
//
// It #includes the REAL main/reader_app.c so the static state, key dispatcher
// and pagination helpers can be driven directly. Everything hardware-shaped
// (FreeRTOS, LVGL, BSP, SPIFFS, NVS, Wi-Fi) is replaced by the tiny stubs in
// tests/reader_app_stubs/ and the in-memory store below. This is not a device
// test: it proves navigation, history and data plumbing, while a real stack
// overflow still needs the on-device handoff.
//
// Build from the repository root (tests/test_reader_app_navigation.c is the
// only translation unit; it pulls in the real state machine via #include):
//   cc -std=c11 -Wall -Wextra -Werror -Itests/reader_app_stubs -Imain
//      tests/test_reader_app_navigation.c main/reader_text.c
//      main/reader_layout.c main/reader_storage.c main/reader_progress.c
//      main/reader_bookmark.c main/builtin_books.c -o /tmp/test_reader_app_navigation
#include "builtin_books.h"
#include "reader_app.h"
#include "reader_bookmark.h"
#include "reader_layout.h"
#include "reader_progress.h"
#include "reader_storage.h"
#include "reader_store.h"
#include "reader_text.h"
#include "reader_transfer.h"
#include "reader_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// Hardware / OS stubs
// ---------------------------------------------------------------------------
static int64_t g_now_us;
int64_t stub_now_us(void) { return g_now_us; }
int stub_battery_soc(void) { return -1; }
int stub_battery_mv(void) { return 0; }

// What the UI stub last received, so the test can assert what would be drawn.
static reader_screen_t g_rendered;
static int g_last_mark_count;
static char g_last_mark_titles[READER_BOOKMARK_MAX][READER_BOOKMARK_TITLE_SIZE];
static char g_storage_hint[128];
static size_t g_storage_total, g_storage_used, g_storage_free;
static char g_confirm_question[48];
static char g_confirm_item[READER_BOOK_TITLE_MAX];
static char g_finished_title[READER_BOOK_TITLE_MAX];
static char g_cover_title[READER_BOOK_TITLE_MAX];
static size_t g_cover_chars;
static size_t g_cover_bytes;
// Host tests exercise the "already reading" path by default; set to 1 to
// simulate a book that has never been opened (no progress record).
static int g_force_first_open;

void reader_ui_init(void) { }
void reader_ui_deinit(void) { }
void reader_ui_set_dark(bool dark) { (void)dark; }
void reader_ui_set_hint(const char *text) { (void)text; }
void reader_ui_set_battery(const char *text) { (void)text; }

void reader_ui_render_library(const reader_book_info_t *books, int count,
                              int selected, const char *battery)
{
    (void)books; (void)count; (void)selected; (void)battery;
    g_rendered = READER_SCREEN_LIBRARY;
}
void reader_ui_render_reader(const char *title, const char *chapter,
                             const char *page_text, int font_size, int percent,
                             const char *battery, int auto_mode, int immersive)
{
    (void)title; (void)chapter; (void)page_text; (void)font_size; (void)percent;
    (void)battery; (void)auto_mode; (void)immersive;
    g_rendered = READER_SCREEN_READER;
}
void reader_ui_render_toc(const reader_book_index_t *index, int mark_count,
                          const char *const *mark_titles, int mode, int focus,
                          int selected, const char *battery)
{
    (void)index; (void)mode; (void)focus; (void)selected; (void)battery;
    g_rendered = READER_SCREEN_TOC;
    g_last_mark_count = mark_count;
    for (int i = 0; i < READER_BOOKMARK_MAX; ++i) {
        if (mark_titles && mark_titles[i]) {
            snprintf(g_last_mark_titles[i], READER_BOOKMARK_TITLE_SIZE, "%s",
                     mark_titles[i]);
        } else {
            g_last_mark_titles[i][0] = '\0';
        }
    }
}
void reader_ui_render_settings(int selected, int font_size, int backlight,
                               int idle_seconds, int dark, int immersive,
                               const char *battery)
{
    (void)selected; (void)font_size; (void)backlight; (void)idle_seconds;
    (void)dark; (void)immersive; (void)battery;
    g_rendered = READER_SCREEN_SETTINGS;
}
void reader_ui_render_font_picker(int font_size, const char *battery)
{
    (void)font_size; (void)battery;
    g_rendered = READER_SCREEN_FONT_PICKER;
}
void reader_ui_render_finished(const char *book_title)
{
    g_rendered = READER_SCREEN_FINISHED;
    snprintf(g_finished_title, sizeof(g_finished_title), "%s",
             book_title ? book_title : "");
}
void reader_ui_render_cover(const char *book_title, size_t char_count,
                            size_t book_bytes)
{
    g_rendered = READER_SCREEN_COVER;
    g_cover_chars = char_count;
    g_cover_bytes = book_bytes;
    snprintf(g_cover_title, sizeof(g_cover_title), "%s",
             book_title ? book_title : "");
}
void reader_ui_render_confirm(const char *question, const char *item,
                              const char *battery)
{
    (void)battery;
    g_rendered = READER_SCREEN_CONFIRM;
    snprintf(g_confirm_question, sizeof(g_confirm_question), "%s",
             question ? question : "");
    snprintf(g_confirm_item, sizeof(g_confirm_item), "%s", item ? item : "");
}
void reader_ui_render_storage(size_t total, size_t used, size_t free_bytes,
                              const char *hint, const char *battery)
{
    (void)battery;
    g_rendered = READER_SCREEN_STORAGE;
    g_storage_total = total;
    g_storage_used = used;
    g_storage_free = free_bytes;
    snprintf(g_storage_hint, sizeof(g_storage_hint), "%s", hint ? hint : "");
}
void reader_ui_render_transfer(const reader_transfer_status_t *status,
                               size_t total, size_t used, size_t free_bytes,
                               const char *battery)
{
    (void)status; (void)total; (void)used; (void)free_bytes; (void)battery;
    g_rendered = READER_SCREEN_TRANSFER;
}
void reader_ui_refresh_transfer(const reader_transfer_status_t *status,
                                size_t total, size_t used, size_t free_bytes)
{
    (void)status; (void)total; (void)used; (void)free_bytes;
}

esp_err_t reader_transfer_start(void) { return ESP_OK; }
void reader_transfer_stop(void) { }
void reader_transfer_get_status(reader_transfer_status_t *out)
{
    memset(out, 0, sizeof(*out));
}

// ---------------------------------------------------------------------------
// In-memory stand-in for the SPIFFS + NVS store
// ---------------------------------------------------------------------------
static const char *const UPLOAD_TEXT =
    "第一章 起程\n"
    "\n"
    "清晨的雾还没有散，河面上浮着一层薄薄的白。"
    "他把行李放上船头，回头望了一眼来时的路。\n"
    "\n"
    "第二章 途中\n"
    "\n"
    "船过桥洞时，水声忽然变得很轻。"
    "岸上有人叫他的名字，他没有回头。\n";
static const char *const UPLOAD_ID = "novel.txt";
static int g_has_upload;
// Mirrors the NVS "hidden" byte in reader_store.c: bit i hides sample i.
static uint8_t g_hidden_builtins;

static reader_progress_blob_t g_progress;
static reader_bookmark_blob_t g_bookmarks;
static uint8_t g_settings[16];

static const char *text_for(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < g_builtin_book_count; ++i) {
        if (strcmp(id, g_builtin_books[i].id) == 0) return g_builtin_books[i].text;
    }
    if (strcmp(id, UPLOAD_ID) == 0) return UPLOAD_TEXT;
    return NULL;
}

static uint8_t *setting_slot(const char *key)
{
    unsigned h = 0;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p) {
        h = h * 31u + *p;
    }
    return &g_settings[h % sizeof(g_settings)];
}

int reader_store_list(reader_book_info_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_builtin_book_count && n < max; ++i) {
        if (i > 0 && i < 8 && (g_hidden_builtins & (1u << i))) continue;
        snprintf(out[n].id, sizeof(out[n].id), "%s", g_builtin_books[i].id);
        snprintf(out[n].title, sizeof(out[n].title), "%s", g_builtin_books[i].title);
        out[n].size = (uint32_t)strlen(g_builtin_books[i].text);
        out[n].builtin = true;
        ++n;
    }
    if (g_has_upload && n < max) {
        snprintf(out[n].id, sizeof(out[n].id), "%s", UPLOAD_ID);
        snprintf(out[n].title, sizeof(out[n].title), "%s", "测试小说");
        out[n].size = (uint32_t)strlen(UPLOAD_TEXT);
        out[n].builtin = false;
        ++n;
    }
    return n;
}

esp_err_t reader_store_space(size_t *total, size_t *used, size_t *free_bytes)
{
    if (total) *total = 4u * 1024u * 1024u;
    if (used) *used = 4096u;
    if (free_bytes) *free_bytes = 4u * 1024u * 1024u - 4096u;
    return ESP_OK;
}

int reader_store_read(const char *id, size_t offset, char *buf, size_t len)
{
    const char *text = text_for(id);
    if (!text || offset >= strlen(text)) return 0;
    size_t total = strlen(text);
    size_t n = total - offset;
    if (n > len) n = len;
    memcpy(buf, text + offset, n);
    return (int)n;
}

esp_err_t reader_store_build_index(const char *id, reader_book_index_t *out)
{
    memset(out, 0, sizeof(*out));
    char chunk[256], line[192];
    size_t line_len = 0, line_start = 0, absolute = 0;
    while (out->count < READER_MAX_CHAPTERS) {
        int n = reader_store_read(id, absolute, chunk, sizeof(chunk));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            char c = chunk[i];
            size_t pos = absolute + (size_t)i;
            unsigned char uc = (unsigned char)c;
            if ((uc & 0xC0u) != 0x80u && uc != ' ' && uc != '\t' &&
                !(pos < 3 && (uc == 0xEFu || uc == 0xBBu || uc == 0xBFu))) {
                ++out->char_count;
            }
            if (c == '\n' || c == '\r') {
                if (line_len > 0 && reader_chapter_match(line, line_len)) {
                    out->items[out->count].offset = line_start;
                    size_t k = 0;
                    while (k < line_len && k + 1 < sizeof(out->items[0].title)) {
                        out->items[out->count].title[k] = line[k]; ++k;
                    }
                    out->items[out->count].title[k] = '\0';
                    ++out->count;
                }
                line_len = 0;
                line_start = pos + 1;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
        }
        absolute += (size_t)n;
        if ((size_t)n < sizeof(chunk) && line_len > 0 &&
            reader_chapter_match(line, line_len)) {
            out->items[out->count].offset = line_start;
            out->items[out->count].title[0] = '\0';
            ++out->count;
        }
    }
    if (out->count == 0) {
        out->items[0].offset = 0;
        snprintf(out->items[0].title, sizeof(out->items[0].title), "%s", "开篇");
        out->count = 1;
    }
    return ESP_OK;
}

bool reader_store_has_progress(const char *id)
{
    (void)id;
    // Default: pretend a progress record exists so the existing navigation
    // tests keep taking the "resume reading" path. g_force_first_open flips
    // this to exercise the first-open title card.
    return g_force_first_open ? false : true;
}
uint32_t reader_store_get_offset(const char *id, uint16_t *chapter_out)
{
    reader_progress_entry_t e;
    if (chapter_out) *chapter_out = 0;
    if (reader_progress_find(&g_progress, reader_book_id(id), &e)) {
        if (chapter_out) *chapter_out = e.chapter;
        return e.offset;
    }
    return 0;
}
void reader_store_set_offset(const char *id, uint32_t offset, uint16_t chapter)
{
    reader_progress_entry_t e = {
        .book_id = reader_book_id(id), .offset = offset,
        .chapter = chapter, .flags = 0
    };
    reader_progress_set(&g_progress, &e);
}
uint8_t reader_store_get_setting(const char *key, uint8_t fallback)
{
    uint8_t v = *setting_slot(key);
    return v ? v : fallback;
}
void reader_store_set_setting(const char *key, uint8_t value)
{
    *setting_slot(key) = value;
}

int reader_store_bookmark_list(uint32_t book_id, reader_bookmark_entry_t *out,
                               int max)
{
    return reader_bookmark_list(&g_bookmarks, book_id, out, max);
}
int reader_store_bookmark_toggle(uint32_t book_id, uint32_t offset,
                                 uint16_t chapter)
{
    if (reader_bookmark_find(&g_bookmarks, book_id, offset) >= 0) {
        reader_bookmark_remove(&g_bookmarks, book_id, offset);
        return 0;
    }
    reader_bookmark_entry_t e = {
        .book_id = book_id, .offset = offset, .chapter = chapter, .flags = 0
    };
    return reader_bookmark_add(&g_bookmarks, &e) > 0 ? 1 : 0;
}
int reader_store_bookmark_remove(uint32_t book_id, uint32_t offset)
{
    return reader_bookmark_remove(&g_bookmarks, book_id, offset);
}
int reader_store_bookmark_excerpt(const char *id, uint32_t offset,
                                  char *out, size_t out_size)
{
    char window[256];
    int n = reader_store_read(id, (size_t)offset, window, sizeof(window));
    if (n <= 0) { out[0] = '\0'; return 0; }
    return reader_excerpt(window, (size_t)n, 20, out, out_size);
}
static void purge_book(const char *id)
{
    uint32_t bid = reader_book_id(id);
    reader_progress_remove(&g_progress, bid);
    reader_bookmark_remove_book(&g_bookmarks, bid);
}

esp_err_t reader_store_delete_book(const char *id)
{
    if (!id || id[0] == '\0') return ESP_ERR_INVALID_ARG;
    // The guide is the only protected built-in.
    if (strcmp(id, READER_BUILTIN_GUIDE_ID) == 0) return ESP_ERR_NOT_SUPPORTED;
    if (strncmp(id, "builtin:", 8) == 0) {
        int found = 0;
        for (int i = 1; i < g_builtin_book_count; ++i) {
            if (strcmp(id, g_builtin_books[i].id) == 0) {
                g_hidden_builtins |= (uint8_t)(1u << i);
                found = 1;
                break;
            }
        }
        if (!found) return ESP_ERR_NOT_FOUND;
    } else {
        if (!g_has_upload || strcmp(id, UPLOAD_ID) != 0) return ESP_ERR_NOT_FOUND;
        g_has_upload = 0;
    }
    purge_book(id);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// The real application state machine
// ---------------------------------------------------------------------------
#include "reader_app.c"

static void key(int btn, int event)
{
    g_now_us += 100000;
    input_event_t in = { .btn = btn, .event = event };
    process_key(&in);
}

static void tap(int btn) { key(btn, BSP_BTN_CLICK); }

// A long press arms a 400 ms suppression window; step past it so the next
// short press is not swallowed.
static void hold(int btn)
{
    key(btn, BSP_BTN_LONG);
    g_now_us += 500000;
}

// Row 0 of the shelf is the first book (the guide), so a single OK opens it.
static void open_first_book(void)
{
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_READER);
}

static void reset_fixture(void)
{
    reader_app_deinit();
    g_now_us = 0;
    g_has_upload = 1;
    g_hidden_builtins = 0;
    reader_progress_init(&g_progress);
    reader_bookmark_init(&g_bookmarks);
    memset(g_settings, 0, sizeof(g_settings));
    memset(g_last_mark_titles, 0, sizeof(g_last_mark_titles));
    memset(g_storage_hint, 0, sizeof(g_storage_hint));
    memset(g_confirm_question, 0, sizeof(g_confirm_question));
    memset(g_confirm_item, 0, sizeof(g_confirm_item));
    memset(g_finished_title, 0, sizeof(g_finished_title));
    memset(g_cover_title, 0, sizeof(g_cover_title));
    g_cover_chars = g_cover_bytes = 0;
    g_force_first_open = 0;
    g_last_mark_count = 0;
    g_storage_total = g_storage_used = g_storage_free = 0;
    s_hint[0] = '\0';
    s_hint_until_ms = 0;
    s_ignore_until_us = 0;
    CHECK(reader_app_init(NULL) == ESP_OK);
}

// Regression for the reboot: a finished book is reopened with an EMPTY back
// history, so the first UP used to nest two ~2 KB pagination windows on a
// 4 KB task stack and overflow it. Keep stepping back and assert the state
// stays continuous instead of resetting.
static void test_finished_book_up_does_not_reboot(void)
{
    reset_fixture();
    open_first_book();

    int guard = 0;
    while (!at_book_end() && guard++ < 500) tap(BSP_BTN_DOWN);
    CHECK(guard < 500);
    CHECK(read_percent() == 100);

    tap(BSP_BTN_DOWN);
    CHECK(s_state.screen == READER_SCREEN_FINISHED);
    // The "完" page now carries the book name as its subtitle.
    CHECK(g_finished_title[0] != '\0');
    CHECK(strcmp(g_finished_title, s_state.current_title) == 0);
    tap(BSP_BTN_DOWN);
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);

    uint32_t saved = g_progress.entries[0].offset;
    CHECK(saved > 0);
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_READER);
    CHECK(s_state.page_offset == saved);
    CHECK(read_percent() == 100);

    uint32_t before = s_state.page_offset;
    tap(BSP_BTN_UP);
    CHECK(s_state.screen == READER_SCREEN_READER);
    CHECK(s_state.page_offset < before);
    CHECK(s_state.page_text[0] != '\0');
    CHECK(read_percent() < 100);

    uint32_t prev = s_state.page_offset;
    for (int i = 0; i < 200 && s_state.page_offset > 0; ++i) {
        tap(BSP_BTN_UP);
        CHECK(s_state.page_offset <= prev);
        CHECK(s_state.page_offset < s_state.book_size);
        prev = s_state.page_offset;
    }
    CHECK(s_state.page_offset == 0);
}

// A book with no saved progress shows its title first, then waits for a key.
// The existing resume path must skip the card entirely.
static void test_first_open_cover(void)
{
    reset_fixture();
    g_force_first_open = 1;

    tap(BSP_BTN_OK);                       // open the guide from the shelf
    CHECK(s_state.screen == READER_SCREEN_COVER);
    CHECK(g_rendered == READER_SCREEN_COVER);
    CHECK(g_cover_title[0] != '\0');
    CHECK(strcmp(g_cover_title, s_state.current_title) == 0);
    // The subtitle is book data: readable size plus a real word count.
    CHECK(g_cover_bytes == s_state.book_size);
    CHECK(g_cover_chars == s_state.book_chars);
    CHECK(g_cover_chars > 0);

    hold(BSP_BTN_OK);                      // long OK backs out, no reading
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);

    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_COVER);
    tap(BSP_BTN_DOWN);                    // short DOWN starts reading
    CHECK(s_state.screen == READER_SCREEN_READER);

    // UP and OK must not be dead ends on the cover either.
    g_force_first_open = 0;               // now simulate an existing record
    hold(BSP_BTN_OK);                      // leave the book
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);
    g_force_first_open = 1;
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_COVER);
    tap(BSP_BTN_UP);
    CHECK(s_state.screen == READER_SCREEN_READER);

    g_force_first_open = 0;
    hold(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);
    tap(BSP_BTN_OK);                      // resume path: no title card
    CHECK(s_state.screen == READER_SCREEN_READER);
    CHECK(g_rendered == READER_SCREEN_READER);
}

// The title card is page 0, not just a first-open splash: pressing UP on the
// first text page must reach it, and DOWN must return to the same page.
static void test_cover_is_page_zero(void)
{
    reset_fixture();
    g_force_first_open = 0;              // resume path: open straight onto text

    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_READER);
    CHECK(s_state.page_offset == 0);
    const uint32_t first_page = s_state.page_offset;

    // The book may hold several pages; walk UP until the card appears.
    for (int i = 0; i < 16 && s_state.screen != READER_SCREEN_COVER; ++i) {
        tap(BSP_BTN_UP);
    }
    CHECK(s_state.screen == READER_SCREEN_COVER);
    CHECK(g_rendered == READER_SCREEN_COVER);
    CHECK(strcmp(g_cover_title, s_state.current_title) == 0);
    CHECK(g_cover_bytes == s_state.book_size);
    CHECK(g_cover_chars > 0);

    tap(BSP_BTN_DOWN);
    CHECK(s_state.screen == READER_SCREEN_READER);
    CHECK(s_state.page_offset == first_page);
}

// A second guard: the two functions that own big pagination windows must not
// call another big-window function while their own frame is live. This is the
// structural invariant behind the crash, checked against the real source.
static void test_no_nested_pagination_windows(void)
{
    const char *self = __FILE__;
    char dir[512] = ".";
    const char *slash = strrchr(self, '/');
    if (slash) {
        size_t n = (size_t)(slash - self);
        if (n < sizeof(dir)) { memcpy(dir, self, n); dir[n] = '\0'; }
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/../main/reader_app.c", dir);
    FILE *fp = fopen(path, "rb");
    if (!fp) { CHECK(0); return; }
    static char src[128 * 1024];
    size_t len = fread(src, 1, sizeof(src) - 1, fp);
    src[len] = '\0';
    fclose(fp);

    const char *a = strstr(src, "static int prev_chapter_last_page");
    const char *b = strstr(src, "static void reflow_current_page");
    CHECK(a != NULL && b != NULL);
    if (!a || !b) return;

    const char *a_end = strstr(a + 1, "\nstatic ");
    const char *b_end = strstr(b + 1, "\nstatic ");
    CHECK(a_end != NULL && b_end != NULL);
    if (!a_end || !b_end) return;

    char a_body[4096], b_body[4096];
    size_t an = (size_t)(a_end - a), bn = (size_t)(b_end - b);
    if (an >= sizeof(a_body)) an = sizeof(a_body) - 1;
    if (bn >= sizeof(b_body)) bn = sizeof(b_body) - 1;
    memcpy(a_body, a, an); a_body[an] = '\0';
    memcpy(b_body, b, bn); b_body[bn] = '\0';
    CHECK(strstr(a_body, "rebuild_history(") == NULL);
    CHECK(strstr(b_body, "PAGE_WINDOW_SIZE") == NULL);

    // A first open renders the title card BEFORE the slow chapter-index build.
    CHECK(strstr(src, "const bool first_open = !reader_store_has_progress") != NULL);
    CHECK(strstr(src, "s_state.screen = first_open ? READER_SCREEN_COVER") != NULL);
    const char *cover = strstr(src, "s_state.screen = READER_SCREEN_COVER;\n        render_screen();");
    const char *index = strstr(src, "reader_store_build_index(s_state.current_id");
    CHECK(cover != NULL && index != NULL && cover < index);
}

// Bookmark labels must be the sentence at the saved offset, and deleting one
// now goes through the confirmation page (cancel paths do NOT delete).
static void test_bookmark_excerpt_and_delete(void)
{
    reset_fixture();
    open_first_book();
    tap(BSP_BTN_DOWN);
    tap(BSP_BTN_DOWN);
    hold(BSP_BTN_UP);
    CHECK(s_state.bookmark_count == 1);
    CHECK(strstr(s_hint, "已添加书签") != NULL);

    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_TOC);
    tap(BSP_BTN_DOWN);            // tab row -> bookmark tab
    CHECK(s_state.toc_mode == 1);
    tap(BSP_BTN_OK);              // -> bookmark list
    CHECK(s_state.toc_focus == 1);
    CHECK(s_state.bookmark_count == 1);
    CHECK(g_last_mark_count == 1);

    const char *label = s_state.bookmark_titles[0];
    CHECK(label[0] != '\0');
    CHECK(strcmp(label, chapter_title()) != 0);
    CHECK(strcmp(g_last_mark_titles[0], label) == 0);
    char first[4] = { 0 };
    memcpy(first, label, 3);
    CHECK(strstr(s_state.page_text, first) != NULL);

    // A long press only opens the confirmation; the bookmark is still there.
    hold(BSP_BTN_UP);
    CHECK(s_state.screen == READER_SCREEN_CONFIRM);
    CHECK(g_rendered == READER_SCREEN_CONFIRM);
    CHECK(s_state.confirm_kind == 1);
    CHECK(s_state.confirm_from == READER_SCREEN_TOC);
    CHECK(s_state.bookmark_count == 1);
    CHECK(strstr(g_confirm_question, "删除书签") != NULL);
    CHECK(strcmp(g_confirm_item, label) == 0);

    // Short DOWN cancels without touching the store.
    tap(BSP_BTN_DOWN);
    CHECK(s_state.screen == READER_SCREEN_TOC);
    CHECK(s_state.bookmark_count == 1);

    // A long press on the confirmation page is also a cancel.
    hold(BSP_BTN_UP);
    CHECK(s_state.screen == READER_SCREEN_CONFIRM);
    hold(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_TOC);
    CHECK(s_state.bookmark_count == 1);

    // OK is the only key that actually deletes, and it returns to the TOC.
    hold(BSP_BTN_UP);
    CHECK(s_state.screen == READER_SCREEN_CONFIRM);
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_TOC);
    CHECK(s_state.bookmark_count == 0);
    CHECK(strstr(s_hint, "已删除书签") != NULL);
    hold(BSP_BTN_OK);            // long OK returns to the reader
    CHECK(s_state.screen == READER_SCREEN_READER);
}

// Shelf rows: 0..count-1 = books, count = pinned 设置. 接收新书 now lives at
// the TOP of settings and returns to settings when the hotspot is closed.
static void test_pinned_action_rows(void)
{
    reset_fixture();
    CHECK(s_state.book_count == g_builtin_book_count + 1);
    CHECK(s_state.selected == 0);
    // Two built-ins (protected guide + one deletable novel). Shelf scrolling
    // is still exercised by the uploaded book that reset_fixture() adds.
    CHECK(g_builtin_book_count == 2);

    // Row 0 is a real book now, not the hotspot.
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_READER);
    hold(BSP_BTN_OK);            // close the book
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);

    // UP from row 0 wraps to the pinned settings row.
    tap(BSP_BTN_UP);
    CHECK(s_state.selected == s_state.book_count);
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_SETTINGS);
    CHECK(s_state.setting_selected == 0);

    // Settings row 0 is 接收新书 -> hotspot, OK-long returns to SETTINGS.
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_TRANSFER);
    CHECK(s_state.transfer_from == READER_SCREEN_SETTINGS);
    hold(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_SETTINGS);

    // Settings has 7 rows; the last one is the storage sub-page.
    for (int i = 0; i < 6; ++i) tap(BSP_BTN_DOWN);
    CHECK(s_state.setting_selected == 6);
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_STORAGE);
    CHECK(g_rendered == READER_SCREEN_STORAGE);
    CHECK(strstr(g_storage_hint, "约可再存") != NULL);
    CHECK(g_storage_free > 0);
    CHECK(g_storage_total > g_storage_used);
    hold(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_SETTINGS);
}

// Only the guide is protected; every other sample and the upload are
// deletable, and each deletion runs through the confirmation page.
static void test_deletable_builtin_samples(void)
{
    reset_fixture();
    int before = s_state.book_count;   // 2 built-ins + 1 uploaded book

    // The pinned settings row is not a book.
    s_state.selected = s_state.book_count;
    hold(BSP_BTN_UP);
    CHECK(strstr(s_hint, "此项不可删除") != NULL);
    CHECK(s_state.book_count == before);
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);

    // The guide is protected too.
    s_state.selected = 0;
    hold(BSP_BTN_DOWN);
    CHECK(strstr(s_hint, "使用指南不可删除") != NULL);
    CHECK(s_state.book_count == before);

    // Always delete row 1: after each removal the next sample shifts up, so
    // this walks through every deletable book including the uploaded one.
    for (int remaining = before; remaining > 1; --remaining) {
        s_state.selected = 1;
        CHECK(strcmp(s_state.books[1].id, READER_BUILTIN_GUIDE_ID) != 0);
        char victim[READER_BOOK_ID_MAX];
        snprintf(victim, sizeof(victim), "%s", s_state.books[1].id);
        char title[READER_BOOK_TITLE_MAX];
        snprintf(title, sizeof(title), "%s", s_state.books[1].title);

        hold(BSP_BTN_DOWN);
        CHECK(s_state.screen == READER_SCREEN_CONFIRM);
        CHECK(s_state.confirm_kind == 2);
        CHECK(s_state.confirm_from == READER_SCREEN_LIBRARY);
        CHECK(strcmp(s_state.confirm_id, victim) == 0);
        CHECK(strcmp(s_state.confirm_label, title) == 0);
        CHECK(strcmp(g_confirm_item, title) == 0);
        CHECK(strstr(g_confirm_question, "删除书籍") != NULL);
        CHECK(s_state.book_count == remaining);   // not deleted yet

        tap(BSP_BTN_OK);
        CHECK(s_state.screen == READER_SCREEN_LIBRARY);
        CHECK(s_state.book_count == remaining - 1);
        CHECK(strstr(s_hint, "已删除本书") != NULL);
    }

    // "仅保留使用指南一本内置书": the shelf ends up with just the guide.
    CHECK(s_state.book_count == 1);
    CHECK(strcmp(s_state.books[0].id, READER_BUILTIN_GUIDE_ID) == 0);
    CHECK(g_has_upload == 0);
    s_state.selected = 0;
    tap(BSP_BTN_OK);
    CHECK(s_state.screen == READER_SCREEN_READER);
    CHECK(strcmp(s_state.current_id, READER_BUILTIN_GUIDE_ID) == 0);
    CHECK(s_state.current_title[0] != '\0');

    hold(BSP_BTN_OK);            // reader long OK closes the book
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);
    s_state.selected = s_state.book_count;
    hold(BSP_BTN_OK);            // shelf root: OK-long resets the cursor
    CHECK(s_state.selected == 0);
}

// Cancelling a book deletion must leave the store completely untouched.
static void test_delete_confirmation_cancel(void)
{
    reset_fixture();
    int before = s_state.book_count;

    s_state.selected = 1;        // the single deletable novel
    hold(BSP_BTN_DOWN);          // -> confirmation
    CHECK(s_state.screen == READER_SCREEN_CONFIRM);
    CHECK(s_state.confirm_kind == 2);
    CHECK(strcmp(s_state.confirm_id, "builtin:ginga") == 0);

    tap(BSP_BTN_UP);             // cancel
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);
    CHECK(s_state.book_count == before);
    CHECK(s_state.confirm_kind == 0);
    CHECK(strcmp(s_state.books[1].id, "builtin:ginga") == 0);

    s_state.selected = 1;
    hold(BSP_BTN_DOWN);
    CHECK(s_state.screen == READER_SCREEN_CONFIRM);
    hold(BSP_BTN_DOWN);          // long press on confirm = cancel
    CHECK(s_state.screen == READER_SCREEN_LIBRARY);
    CHECK(s_state.book_count == before);
    CHECK(strcmp(s_state.books[1].id, "builtin:ginga") == 0);
}

// The UI layer has no host renderer, so assert its layout contract against the
// real source: two-chips hint, one pinned settings row, seven settings rows,
// the confirmation page, the storage header, and the "完" title subtitle.
static void test_shelf_ui_contract(void)
{
    const char *self = __FILE__;
    char dir[512] = ".";
    const char *slash = strrchr(self, '/');
    if (slash) {
        size_t n = (size_t)(slash - self);
        if (n < sizeof(dir)) { memcpy(dir, self, n); dir[n] = '\0'; }
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/../main/reader_ui.c", dir);
    FILE *fp = fopen(path, "rb");
    if (!fp) { CHECK(0); return; }
    static char src[128 * 1024];
    size_t len = fread(src, 1, sizeof(src) - 1, fp);
    src[len] = '\0';
    fclose(fp);

    CHECK(strstr(src, "selected == count") != NULL);          // bottom pinned
    CHECK(strstr(src, "int list_index = start + i;") != NULL); // books start at 0
    CHECK(strstr(src, "is_action ? C_ACTION : C_INK") != NULL); // distinct colour
    CHECK(strstr(src, "footer(scr, \"上/下 选择 长按删除\", \"确定 打开\")") != NULL);
    CHECK(strstr(src, "footer3(") == NULL);                   // back to two chips
    CHECK(strstr(src, "%d 本书 · 长按上/下删除") == NULL);      // no header hint
    CHECK(strstr(src, "const char *names[7] = { \"接收新书\"") != NULL);
    CHECK(strstr(src, "header(scr, \"存储空间\", NULL, battery)") != NULL);
    CHECK(strstr(src, "header(scr, \"存储空间\", \"书籍分区\"") == NULL);
    CHECK(strstr(src, "void reader_ui_render_finished(const char *book_title)") != NULL);
    CHECK(strstr(src, "void reader_ui_render_confirm(") != NULL);
    // Title card shares the 完 page's centred typography, but its subtitle is
    // book data (字数 + readable size), never the app name.
    CHECK(strstr(src, "void reader_ui_render_cover(const char *book_title, size_t char_count,") != NULL);
    CHECK(strstr(src, "%u字 · %s") != NULL);
    CHECK(strstr(src, "lv_label_set_text(sub, \"掌小读\")") == NULL);
    CHECK(strstr(src, "内置示例 · %u B") == NULL);   // shelf uses readable units
    CHECK(strstr(src, "footer(scr, \"按 确定/下 开始阅读\", NULL)") != NULL);
    CHECK(strstr(src, "删除后无法恢复") != NULL);

    // Quiet UI: the transfer and font-picker headers carry no caption now.
    CHECK(strstr(src, "header(scr, \"接收新书\", NULL, battery)") != NULL);
    CHECK(strstr(src, "header(scr, \"正文字号\", NULL, battery)") != NULL);
    CHECK(strstr(src, "手机连热点后打开网页上传") == NULL);
    CHECK(strstr(src, "与阅读页面一致") == NULL);
    CHECK(strstr(src, "reader_ui_render_font_picker(int font_size, const char *battery)") != NULL);
    CHECK(strstr(src, "reader_ui_render_font_picker(int font_size, int immersive") == NULL);

    // The chapter moved up into the header subtitle slot (y=38), no longer a
    // separate sunk caption at y=58.
    CHECK(strstr(src, "header(scr, title ? title : \"阅读\", chapter ? chapter : NULL, battery)") != NULL);
    CHECK(strstr(src, "label(scr, UI_X, 58") == NULL);

    // After two unsatisfactory greens the action rows are back to the cool
    // blue the reader already approved; no green value may remain in either
    // palette. 接收新书 keeps the same C_ACTION emphasis.
    CHECK(strstr(src, "0x3F6C93") != NULL);
    CHECK(strstr(src, "0x8FB3D9") != NULL);
    CHECK(strstr(src, "0x5E8365") == NULL);
    CHECK(strstr(src, "0xA7C4A9") == NULL);
    CHECK(strstr(src, "0x4F8A59") == NULL);
    CHECK(strstr(src, "0x9FCFA3") == NULL);
    CHECK(strstr(src, "i == 0 ? C_ACTION : C_INK") != NULL);

    // Body grew one line: it now starts at y=68 (not 84), and BODY_H=188 is
    // exactly the gap to the 已读 label at 256. The immersive box reaches 288.
    CHECK(strstr(src, "lv_obj_set_pos(body, UI_X, 68)") != NULL);
    CHECK(strstr(src, "lv_obj_set_pos(body, UI_X, 84)") == NULL);
    CHECK(READER_LAYOUT_BODY_H == 188);
    CHECK(READER_LAYOUT_IMMERSIVE_TOP == 16);
    CHECK(READER_LAYOUT_IMMERSIVE_H == 272);
    CHECK(READER_LAYOUT_MAX_PAGE_BYTES == 896);
    CHECK(reader_layout_lines(12) == 11);
    CHECK(reader_layout_immersive_lines(12) == 16);
    CHECK(reader_layout_immersive_lines(16) == 13);  // default font gains a line
    CHECK(reader_layout_immersive_lines(20) == 11);

    // Transfer page: one centred block, two captions and one status line
    // removed. No "total/used" detail widget, no separate upload row. The
    // progress bar is now drawn by the shared bar() helper; y=202, a 204x4 px
    // track and an empty initial fill still describe the same centred layout.
    CHECK(strstr(src, "bar(scr, UI_X, 202, 204, 4, 0)") != NULL);
    CHECK(strstr(src, "热点名称") == NULL);
    CHECK(strstr(src, "剩余存储空间") == NULL);
    CHECK(strstr(src, "s_t_detail") == NULL);
    CHECK(strstr(src, "s_t_upload") == NULL);
    CHECK(strstr(src, "台设备") == NULL);

    // End-of-book and delete-confirm pages re-centred in the usable band.
    CHECK(strstr(src, "UI_X, 118, UI_W - 2 * UI_X, F_NUM") != NULL);
    CHECK(strstr(src, "UI_X, 120, UI_W - 2 * UI_X, F_BODY") != NULL);
}

// The shipped books must be real, readable content: non-trivial length, at
// least two recognisable chapters, and a paginatable first page. The novel has
// nine 第一卷 headings, so the TOC has real content on a blank device.
static void test_all_builtin_books_readable(void)
{
    CHECK(g_builtin_book_count == 2);
    for (int i = 0; i < g_builtin_book_count; ++i) {
        const char *id = g_builtin_books[i].id;
        size_t bytes = strlen(g_builtin_books[i].text);
        CHECK(bytes > 200);

        reader_book_index_t idx;
        memset(&idx, 0, sizeof(idx));
        CHECK(reader_store_build_index(id, &idx) == ESP_OK);
        CHECK(idx.count >= 2);

        char window[PAGE_WINDOW_SIZE];
        char out[PAGE_TEXT_SIZE];
        int n = reader_store_read(id, 0, window, sizeof(window));
        CHECK(n > 0);
        reader_page_t page;
        CHECK(reader_paginate(window, (size_t)n, 0,
                              reader_layout_cols(READER_FONT_DEFAULT_SIZE),
                              reader_layout_lines(READER_FONT_DEFAULT_SIZE),
                              out, sizeof(out), &page) == 0);
        CHECK(page.next > 0);
        CHECK(out[0] != '\0');
    }
}

int main(void)
{
    test_first_open_cover();
    test_cover_is_page_zero();
    test_finished_book_up_does_not_reboot();
    test_no_nested_pagination_windows();
    test_all_builtin_books_readable();
    test_bookmark_excerpt_and_delete();
    test_pinned_action_rows();
    test_deletable_builtin_samples();
    test_delete_confirmation_cancel();
    test_shelf_ui_contract();
    reader_app_deinit();

    if (failures) {
        printf("reader_app_navigation: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_app_navigation: PASS\n");
    return 0;
}
