// reader_app.c — see reader_app.h.
#include "reader_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "builtin_books.h"
#include "reader_layout.h"
#include "reader_pagemap.h"
#include "reader_progress.h"
#include "reader_storage.h"
#include "reader_store.h"
#include "reader_text.h"
#include "../assets/fonts/reader_ascii_advance.h"
#include "reader_transfer.h"
#include "reader_ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "reader_app";

#define INPUT_QUEUE_DEPTH 12
#define PAGE_WINDOW_SIZE  1024
// An immersive page can hold roughly 50% more lines than a normal one. The
// upper bound is derived and asserted per font size in reader_layout.c, so a
// full page of Hanzi can never be silently truncated by the paginator.
#define PAGE_TEXT_SIZE    READER_LAYOUT_MAX_PAGE_BYTES
#define HISTORY_DEPTH     READER_PAGE_HISTORY_MAX
#define TICK_PERIOD_MS    1000
// Interval used until the reader picks one in 设置 → 自动翻页.
#define AUTO_PAGE_DEFAULT 10
// Hold-to-scroll repeat period. 60 ms is ~16 rows/s: enough to cross a few
// hundred page ranges, slow enough that the full-screen redraw per step keeps up
// on the 80 MHz display bus.
#define SCROLL_PERIOD_MS  60
#define LONG_IGNORE_MS    400
#define BATTERY_TICKS     30
#define PERSIST_EVERY     5
// Anchors kept for the page map: 8 KiB of heap covers 20,480 pages, which is
// far beyond any book that fits the books partition. Past that the page COUNT
// stays exact and only the page-index lookups stop, which the log records.
#define PAGEMAP_ANCHOR_BUDGET 2048u

_Static_assert(PAGE_WINDOW_SIZE >= READER_LAYOUT_MAX_PAGE_BYTES,
               "one full page must fit inside a single read window");

typedef struct {
    int btn;
    int event;
} input_event_t;

typedef struct {
    int screen;
    reader_book_info_t books[READER_MAX_BOOKS];
    int book_count;
    int selected;

    char current_id[READER_BOOK_ID_MAX];
    // Captured when a book opens. The shelf cursor is a LIST row (0..count-1
    // are books, row count is the pinned settings action), and the title is
    // also needed on the "完" page after the reader has left the shelf.
    char current_title[READER_BOOK_TITLE_MAX];
    // Progress/offset space: the ORIGINAL UTF-8 byte length (uncompressed).
    uint32_t book_size;
    // Flash footprint of the compressed book file — shown on the title card
    // so it matches the shelf's "内置示例 · X KB" footprint display.
    uint32_t book_flash_size;
    // Non-whitespace code points, filled by reader_store_build_index() in the
    // same streaming pass that finds chapters; feeds the title-card subtitle.
    size_t book_chars;
    uint32_t page_offset;
    uint32_t next_offset;
    // 1-based page number of page_offset under the CURRENT geometry. Derived,
    // never persisted: a page index means nothing after a font-size change.
    uint32_t page_no;
    char page_text[PAGE_TEXT_SIZE];
    reader_book_index_t index;
    int chapter;
    int toc_selected;

    int setting_selected;
    int font_size;
    int backlight;
    // Auto page-turn interval in seconds; 0 = off. Persisted, and the reading
    // session's live on/off state lives in s_auto_mode.
    int auto_seconds;
    int idle_seconds;
    int dark;
    int immersive;
    int settings_from_reader;

    int toc_mode;    // 0 = chapter list, 1 = bookmark list
    int toc_focus;   // 0 = tab row, 1 = list
    reader_bookmark_entry_t bookmarks[READER_BOOKMARK_MAX];
    int bookmark_count;
    // Labels are generated at load time from the book text AT each bookmark,
    // so two marks in the same chapter no longer look identical.
    char bookmark_titles[READER_BOOKMARK_MAX][READER_BOOKMARK_TITLE_SIZE];
    const char *bookmark_title_ptrs[READER_BOOKMARK_MAX];

    // Delete confirmation: a long press only fills this snapshot and opens
    // READER_SCREEN_CONFIRM; the actual store mutation happens on the second,
    // explicit OK press. `confirm_from` is where a confirm/cancel returns.
    int confirm_kind;    // 0 = none, 1 = bookmark, 2 = book
    int confirm_from;    // reader_screen_t of the calling page
    char confirm_label[READER_BOOK_TITLE_MAX];
    char confirm_id[READER_BOOK_ID_MAX];
    uint32_t confirm_offset;
    // Hotspot page returns to the screen it was opened from (settings or shelf).
    int transfer_from;

    // Page map: total page count plus one byte anchor every
    // READER_PAGEMAP_GROUP pages. Rebuilt whenever the line geometry changes
    // and freed when no book is open.
    reader_pagemap_t pagemap;
    uint32_t *pagemap_anchors;
} reader_app_state_t;

static reader_app_state_t s_state;
// Opening runs on the input worker; the LVGL tick must not replace its overlay.
static atomic_bool s_loading;
static uint32_t s_loading_started;
static uint32_t s_loading_updated;

static uint32_t s_history[HISTORY_DEPTH];
static int s_history_count;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static lv_timer_t *s_tick_timer;
// Fast scrolling for the long TOC lists. Holding UP/DOWN starts it; releasing
// the key (or any other key event) stops it.
static lv_timer_t *s_scroll_timer;
static int s_scroll_dir;
static bool s_ready;
static volatile uint32_t s_last_activity_ms;
static int s_dim_stage;
static int64_t s_ignore_until_us;
static uint32_t s_auto_elapsed_ms;
static int s_auto_mode;
static int s_unpersisted_pages;
static int s_battery_tick;

// Short transient message shown in the reader footer (e.g. "已添加书签").
// 32 B holds eight Hanzi plus NUL, so "使用指南不可删除" is never truncated.
static char s_hint[32];
static uint32_t s_hint_until_ms;

// When the reader footer should go back to its normal key hint.
#define HINT_DURATION_MS 2000

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void note_activity(void)
{
    s_last_activity_ms = now_ms();
    if (s_dim_stage != 0) {
        bsp_display_backlight((uint8_t)s_state.backlight);
        s_dim_stage = 0;
    }
}

static void history_reset(void)
{
    s_history_count = 0;
}

// Append to a bounded list of page starts, evicting the oldest when full.
// Shared by the global back-history and the chapter-walk scratch list so the
// FIFO rule lives in exactly one place.
static void offset_push(uint32_t *arr, int *count, int cap, uint32_t value)
{
    if (*count < cap) {
        arr[(*count)++] = value;
    } else {
        memmove(&arr[0], &arr[1], (size_t)(cap - 1) * sizeof(arr[0]));
        arr[cap - 1] = value;
    }
}

static void history_push(uint32_t offset)
{
    offset_push(s_history, &s_history_count, HISTORY_DEPTH, offset);
}

static int history_pop(uint32_t *offset)
{
    if (s_history_count <= 0) return 0;
    *offset = s_history[--s_history_count];
    return 1;
}

// Page geometry comes straight from reader_layout.c, the same table the font
// picker previews with, so a preview can never disagree with the real page.
static int page_lines(void)
{
    // Immersive mode spends the chrome's vertical space on more lines. Every
    // consumer of a page boundary (pagination, history rebuild, font-size
    // re-flow, bookmark landing) calls this function, so a mode switch cannot
    // leave two consumers disagreeing about where a page ends.
    return s_state.immersive
               ? reader_layout_immersive_lines(s_state.font_size)
               : reader_layout_lines(s_state.font_size);
}

// Prepare the current page text and the next absolute offset. Pure data work:
// no LVGL access, safe to call from the input task.
static int paginate(const char *text, size_t len, size_t start,
                    char *out, size_t out_size, reader_page_t *page)
{
    int font = reader_layout_font_index(s_state.font_size);
    return reader_paginate_fit(text, len, start, reader_layout_cols(s_state.font_size),
                               page_lines(), s_state.font_size, READER_LAYOUT_BODY_W,
                               reader_ascii_advance[font], out, out_size, page);
}

// Both are defined further down; prepare_page() keeps the page number current
// and the page-map pass reports through the same overlay the first open uses.
static void show_loading_progress(const char *phase, size_t done, size_t total);
static void update_page_number(void);
static uint32_t history_layout_key(void);

static void prepare_page(void)
{
    char window[PAGE_WINDOW_SIZE];
    int n = reader_store_read(s_state.current_id, s_state.page_offset,
                              window, sizeof(window));
    if (n <= 0) {
        s_state.page_text[0] = '\0';
        s_state.next_offset = s_state.page_offset;
        return;
    }
    reader_page_t page;
    if (paginate(window, (size_t)n, 0,
                        s_state.page_text, sizeof(s_state.page_text),
                        &page) != 0) {
        s_state.page_text[0] = '\0';
        s_state.next_offset = s_state.page_offset;
        return;
    }
    s_state.next_offset = s_state.page_offset + (uint32_t)page.next;
    if (s_state.next_offset > s_state.book_size) {
        s_state.next_offset = s_state.book_size;
    }

    int best = 0;
    for (int i = 0; i < s_state.index.count; ++i) {
        if (s_state.index.items[i].offset <= s_state.page_offset) best = i;
    }
    s_state.chapter = best;
    update_page_number();
}

// One page step for the page map: same read window and same paginator the
// screen uses, so the map can never disagree with what is drawn.
//
// The buffers are static, not stack: this is called up to GROUP-1 times per page
// number lookup, sometimes from inside prepare_page(), and the input task has
// only 6 KiB. Two ~1 KiB frames nested there is exactly the shape that used to
// panic this device (see prev_chapter_last_page). Only one reader instance
// exists and every caller runs on the input task, so the buffers cannot be in
// use twice at once.
static char s_pagemap_window[PAGE_WINDOW_SIZE];
static char s_pagemap_out[PAGE_TEXT_SIZE];

static int page_next_cb(void *ctx, uint32_t offset, uint32_t *next)
{
    (void)ctx;
    if (!s_state.current_id[0] || !next) return 0;
    int n = reader_store_read(s_state.current_id, offset, s_pagemap_window,
                              sizeof(s_pagemap_window));
    if (n <= 0) return 0;
    char *out = s_pagemap_out;
    reader_page_t page;
    if (paginate(s_pagemap_window, (size_t)n, 0, out, sizeof(s_pagemap_out),
                 &page) != 0) return 0;
    if (page.next == 0) return 0;
    uint32_t nxt = offset + (uint32_t)page.next;
    if (nxt <= offset || nxt > s_state.book_size) return 0;
    *next = nxt;
    return 1;
}

static uint32_t s_pagemap_steps;

static void pagemap_progress_cb(void *ctx, uint32_t done, uint32_t total)
{
    show_loading_progress((const char *)ctx, done, total);
    // A whole-book pass on a multi-megabyte novel is seconds of CPU on a single
    // core: yield often enough that IDLE still feeds the task watchdog.
    if ((++s_pagemap_steps & 63u) == 0) vTaskDelay(1);
}

static void pagemap_free(void)
{
    free(s_state.pagemap_anchors);
    s_state.pagemap_anchors = NULL;
    reader_pagemap_init(&s_state.pagemap, NULL, 0);
    s_state.page_no = 0;
}

// (Re)build the page map for the geometry that is active NOW. `phase` is the
// caption shown in the progress overlay ("正在统计页数" / "正在重排").
static void pagemap_rebuild(const char *phase)
{
    if (!s_state.current_id[0] || s_state.book_size == 0) {
        reader_pagemap_init(&s_state.pagemap, NULL, 0);
        s_state.page_no = 0;
        return;
    }
    if (!s_state.pagemap_anchors) {
        s_state.pagemap_anchors = malloc(PAGEMAP_ANCHOR_BUDGET * sizeof(uint32_t));
        if (!s_state.pagemap_anchors) {
            ESP_LOGW(TAG, "页数索引分配失败，页脚只显示百分比");
        }
    }
    reader_pagemap_init(&s_state.pagemap, s_state.pagemap_anchors,
                        s_state.pagemap_anchors ? PAGEMAP_ANCHOR_BUDGET : 0);
    // Pages depend only on the pagination geometry, so an unchanged font size
    // and mode reuse the previous result instead of re-reading the whole book.
    // That is the difference between "opens at once" and "spins for a second".
    if (reader_store_pagemap_load(s_state.current_id, history_layout_key(),
                                  s_state.book_size, &s_state.pagemap)) {
        s_state.page_no = reader_pagemap_page_at(&s_state.pagemap, page_next_cb, NULL,
                                                 s_state.page_offset);
        ESP_LOGI(TAG, "页数缓存命中: 共 %u 页，当前第 %u 页",
                 (unsigned)s_state.pagemap.pages, (unsigned)s_state.page_no);
        return;
    }
    s_pagemap_steps = 0;
    reader_pagemap_build(&s_state.pagemap, page_next_cb, NULL, s_state.book_size,
                         pagemap_progress_cb, (void *)phase);
    if (s_state.pagemap_anchors && s_state.pagemap.truncated) {
        ESP_LOGW(TAG, "页数索引已满（%u 页之后只计页数，不再支持按页跳转）",
                 (unsigned)(s_state.pagemap.groups * READER_PAGEMAP_GROUP));
    }
    s_state.page_no = reader_pagemap_page_at(&s_state.pagemap, page_next_cb, NULL,
                                             s_state.page_offset);
    // Cache it for the next open. Built-in books have no writable id, so the
    // save is a no-op for them (they are small enough to re-page instantly).
    reader_store_pagemap_save(s_state.current_id, history_layout_key(),
                              s_state.book_size, &s_state.pagemap);
    ESP_LOGI(TAG, "页数: 共 %u 页，当前第 %u 页", (unsigned)s_state.pagemap.pages,
             (unsigned)s_state.page_no);
}

static void update_page_number(void)
{
    if (s_state.pagemap.pages == 0 || !s_state.pagemap_anchors) {
        s_state.page_no = 0;
        return;
    }
    s_state.page_no = reader_pagemap_page_at(&s_state.pagemap, page_next_cb, NULL,
                                             s_state.page_offset);
}

// Identity of one pagination geometry. Both caches that survive a reboot are
// keyed by it: the back-history and the page map (see reader_store_pagemap_save).
// Bump the high bits whenever reader_text.c or reader_layout.c changes how pages
// are laid out, otherwise a book reopened after a firmware update keeps page
// numbers and bookmarks computed with the old rules.
static uint32_t history_layout_key(void)
{
    return 0x10000u | ((uint32_t)s_state.font_size << 1) | (s_state.immersive != 0);
}

static void save_resume_history(void)
{
    reader_store_history_save(s_state.current_id, s_state.page_offset, history_layout_key(),
                               s_history, s_history_count);
}

static void persist_progress(void)
{
    reader_store_set_offset(s_state.current_id, s_state.page_offset,
                            (uint16_t)s_state.chapter);
    s_unpersisted_pages = 0;
}

static const char *chapter_title(void)
{
    int i = s_state.chapter;
    if (i < 0 || i >= s_state.index.count) return "";
    return s_state.index.items[i].title;
}

// The fuel gauge is optional hardware. Prefer its SOC register; while a cold
// start leaves that register unready, a coarse Li-ion voltage curve still gives
// useful feedback; when the chip is absent entirely, say so in words instead
// of showing a cryptic placeholder the user cannot interpret.
static char s_battery_text[16];

static int soc_from_voltage(int mv)
{
    static const struct { int mv, pct; } curve[] = {
        { 3300, 0 }, { 3450, 5 }, { 3600, 20 }, { 3750, 45 },
        { 3900, 70 }, { 4050, 90 }, { 4200, 100 },
    };
    if (mv <= curve[0].mv) return 0;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (mv <= curve[i].mv) {
            int span = curve[i].mv - curve[i - 1].mv;
            return curve[i - 1].pct + (mv - curve[i - 1].mv) *
                   (curve[i].pct - curve[i - 1].pct) / span;
        }
    }
    return 100;
}

static const char *battery_text(void)
{
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        snprintf(s_battery_text, sizeof(s_battery_text), "%d%%", soc);
    } else {
        // Read the voltage once: a second I2C transaction could fail between
        // the plausibility check and the conversion.
        int mv = bsp_battery_mv();
        if (mv >= 2500) {
            snprintf(s_battery_text, sizeof(s_battery_text), "~%d%%",
                     soc_from_voltage(mv));
        } else {
            snprintf(s_battery_text, sizeof(s_battery_text), "电量未知");
        }
    }
    return s_battery_text;
}

// The last page ends exactly at the end of the file. Pressing DOWN there shows
// a short confirmation screen ("完") instead of dropping straight to the shelf;
// that screen owns the actual exit.
static bool at_book_end(void)
{
    return s_state.book_size > 0 &&
           s_state.next_offset >= s_state.book_size;
}

static void space_snapshot(size_t *total, size_t *used, size_t *free_b)
{
    if (reader_store_space(total, used, free_b) != ESP_OK) {
        *total = *used = *free_b = 0;
    }
}

// Reading progress measured at the END of the current page: the last page of a
// book starts before the end of the file but finishes exactly at it, which is
// why a start-offset ratio could never reach 100%.
static int read_percent(void)
{
    if (s_state.book_size == 0) return 0;
    if (s_state.next_offset >= s_state.book_size) return 100;
    uint64_t pct = (uint64_t)s_state.next_offset * 100u / s_state.book_size;
    return pct > 100 ? 100 : (int)pct;
}

// Render the whole screen from current state. Caller holds bsp_lvgl_lock().
static void render_screen_locked(void)
{
    const char *battery = battery_text();
    reader_transfer_status_t status;
    size_t total = 0, used = 0, free_b = 0;

    // Must precede the switch: new_screen() reads the palette when it builds.
    reader_ui_set_dark(s_state.dark != 0);
    reader_ui_set_hint(s_hint);
    switch (s_state.screen) {
    case READER_SCREEN_LIBRARY:
        reader_ui_render_library(s_state.books, s_state.book_count,
                                 s_state.selected, battery);
        break;
    case READER_SCREEN_READER:
        reader_ui_render_reader(
            s_state.current_title, chapter_title(),
            s_state.page_text, s_state.font_size,
            read_percent(), (int)s_state.page_no, (int)s_state.pagemap.pages,
            battery, s_auto_mode, s_state.immersive);
        break;
    case READER_SCREEN_TOC:
        reader_ui_render_toc(&s_state.index, s_state.bookmark_count,
                             s_state.bookmark_title_ptrs,
                             (int)s_state.pagemap.groups, (int)s_state.pagemap.pages,
                             s_state.toc_mode, s_state.toc_focus,
                             s_state.toc_selected, battery);
        break;
    case READER_SCREEN_SETTINGS:
        reader_ui_render_settings(s_state.setting_selected, s_state.font_size,
                                  s_state.backlight, s_state.auto_seconds,
                                  s_state.idle_seconds, s_state.dark,
                                  s_state.immersive, battery);
        break;
    case READER_SCREEN_FONT_PICKER:
        reader_ui_render_font_picker(s_state.font_size, battery);
        break;
    case READER_SCREEN_BACKLIGHT:
        reader_ui_render_backlight_picker(s_state.backlight, battery);
        break;
    case READER_SCREEN_AUTOPAGE:
        reader_ui_render_autopage_picker(s_state.auto_seconds, battery);
        break;
    case READER_SCREEN_TRANSFER:
        reader_transfer_get_status(&status);
        space_snapshot(&total, &used, &free_b);
        reader_ui_render_transfer(&status, total, used, free_b, battery);
        break;
    case READER_SCREEN_FINISHED:
        reader_ui_render_finished(s_state.current_title);
        break;
    case READER_SCREEN_COVER:
        // The title card shows the compressed footprint on flash, not the
        // uncompressed byte length used for progress/offset arithmetic.
        reader_ui_render_cover(s_state.current_title, s_state.book_chars,
                               s_state.book_flash_size);
        break;
    case READER_SCREEN_STORAGE: {
        space_snapshot(&total, &used, &free_b);
        char hint[96];
        if (reader_storage_hint(free_b, hint, sizeof(hint)) < 0) hint[0] = '\0';
        reader_ui_render_storage(total, used, free_b, hint, battery);
        break;
    }
    case READER_SCREEN_CONFIRM:
        reader_ui_render_confirm(
            s_state.confirm_kind == 1 ? "删除书签" : "删除书籍",
            s_state.confirm_label, battery);
        break;
    default:
        break;
    }
}

static void render_screen(void)
{
    if (!bsp_lvgl_lock(500)) return;
    render_screen_locked();
    bsp_lvgl_unlock();
}

static void refresh_book_list(void)
{
    s_state.book_count = reader_store_list(s_state.books, READER_MAX_BOOKS);
    // Rows 0..count-1 are books; row `count` is the pinned settings action.
    if (s_state.selected >= s_state.book_count + 1) {
        s_state.selected = 0;
    }
}

static uint32_t chapter_start_offset(int chapter)
{
    if (chapter < 0 || chapter >= s_state.index.count) return 0;
    return (uint32_t)s_state.index.items[chapter].offset;
}

static void show_loading_progress(const char *phase, size_t done, size_t total)
{
    uint32_t now = now_ms();
    if (now - s_loading_started < 150u || now - s_loading_updated < 100u) return;
    if (!bsp_lvgl_lock(20)) return;
    unsigned percent = total ? (unsigned)((uint64_t)done * 100u / total) : 100;
    reader_ui_update_loading(s_state.current_title, phase, percent);
    bsp_lvgl_unlock();
    s_loading_updated = now;
}

// Show the overlay immediately at 0 % and restart the throttle window, so work
// that begins on a settled screen (a font-size change) never leaves a frozen
// page behind while the book is re-paginated.
static void begin_progress(const char *phase)
{
    s_loading_started = now_ms();
    s_loading_updated = s_loading_started;
    if (!bsp_lvgl_lock(100)) return;
    reader_ui_update_loading(s_state.current_title, phase, 0);
    bsp_lvgl_unlock();
}

static void index_loading_progress(size_t done, size_t total, void *ctx)
{
    (void)ctx;
    show_loading_progress("正在加载目录", done, total);
}

// Walk the chapter from `from` up to `to`, pushing every page start onto the
// back history. Without this the first UP press after opening a book or jumping
// to a chapter does nothing, because forward paging used to be the only thing
// that ever pushed history.
static void rebuild_history_walk(uint32_t from, uint32_t to, bool show_progress)
{
    history_reset();
    if (!s_state.current_id[0] || from >= to) return;

    char text[PAGE_WINDOW_SIZE];
    char out[PAGE_TEXT_SIZE];
    uint32_t cur = from;
    // A malformed book must not spin here; a page is at least tens of bytes.
    for (int guard = 0; guard < 256 && cur < to; ++guard) {
        int n = reader_store_read(s_state.current_id, cur, text, sizeof(text));
        if (n <= 0) break;
        reader_page_t page;
        if (paginate(text, (size_t)n, 0,
                            out, sizeof(out), &page) != 0) break;
        if (page.next == 0) break;
        uint32_t next = cur + (uint32_t)page.next;
        if (next > to) break;
        history_push(cur);
        cur = next;
        if (show_progress) {
            show_loading_progress("正在恢复阅读位置", cur - from, to - from);
            if ((guard & 31) == 31) vTaskDelay(1);
        }
    }
    // If the walk stopped short, the recorded page starts do not chain into the
    // current page; discard them rather than turning back to a wrong offset.
    if (cur < to) history_reset();
}

static void rebuild_history(uint32_t from, uint32_t to)
{
    rebuild_history_walk(from, to, false);
}

// Last-resort back step for books without chapter headings (or when the walk
// above hit its guard): paginate a short window that ends at the current page
// and take the last page start before it. This may not reproduce the exact
// previous layout, but it always moves the reader back instead of doing nothing.
static int prev_page_in_window(uint32_t *out)
{
    if (!out || !s_state.current_id[0] || s_state.page_offset == 0) return 0;
    const uint32_t window = PAGE_WINDOW_SIZE;
    uint32_t base = s_state.page_offset > window ? s_state.page_offset - window : 0;

    char text[PAGE_WINDOW_SIZE];
    int n = reader_store_read(s_state.current_id, base, text, sizeof(text));
    if (n <= 0) return 0;

    char page_buf[PAGE_TEXT_SIZE];
    uint32_t best = base;
    uint32_t cur = base;
    for (int guard = 0; guard < 32; ++guard) {
        uint32_t rel = cur - base;
        if (rel >= (uint32_t)n) break;
        reader_page_t page;
        if (paginate(text, (size_t)n, rel,
                            page_buf, sizeof(page_buf), &page) != 0) break;
        if (page.next == 0) break;
        uint32_t next = cur + (uint32_t)page.next;
        if (next >= s_state.page_offset) break;
        best = next;
        cur = next;
    }
    *out = best;
    return best < s_state.page_offset;
}

// When the back history is empty and the page is not the very first of the
// book, step onto the previous chapter's last page and rebuild that chapter's
// history so further UP presses turn back page by page.
//
// STACK BUDGET: this frame owns a 1024 B read window + an 832 B page buffer.
// An earlier version called rebuild_history() at the end, which owns another
// ~2 KB of the same buffers. Nesting the two used ~4 KB of the 4 KB input-task
// stack -> stack overflow -> panic -> device reboot. It showed up exactly when
// a FINISHED book was reopened, because that is the only state whose history
// rebuild stops short and leaves the stack empty. Collect the page starts while
// walking instead, then push them after the traversal: no nested big frame.
static int prev_chapter_last_page(uint32_t *out)
{
    if (!out || s_state.chapter <= 0) return 0;
    uint32_t start = chapter_start_offset(s_state.chapter - 1);
    uint32_t end = chapter_start_offset(s_state.chapter);
    if (end <= start) return 0;

    char text[PAGE_WINDOW_SIZE];
    char page_buf[PAGE_TEXT_SIZE];
    // Sliding window of the most recent page starts; a long chapter must not
    // overflow it, and the newest boundaries are the useful ones for UP.
    uint32_t starts[HISTORY_DEPTH];
    int nstarts = 0;

    uint32_t best = start;
    uint32_t cur = start;
    for (int guard = 0; guard < 512 && cur < end; ++guard) {
        int n = reader_store_read(s_state.current_id, cur, text, sizeof(text));
        if (n <= 0) break;
        reader_page_t page;
        if (paginate(text, (size_t)n, 0,
                            page_buf, sizeof(page_buf), &page) != 0) break;
        if (page.next == 0) break;
        uint32_t next = cur + (uint32_t)page.next;
        if (next >= end) break;
        offset_push(starts, &nstarts, HISTORY_DEPTH, cur);
        best = next;
        cur = next;
    }
    s_state.chapter = s_state.chapter - 1;
    history_reset();
    for (int i = 0; i < nstarts; ++i) history_push(starts[i]);
    *out = best;
    return 1;
}

// `list_index` is a shelf ROW: rows 0..book_count-1 are the books and the
// last row (book_count) is the pinned settings action. Keeping the list index
// in s_state.selected means returning to the shelf highlights the very book
// the reader just left.
static void set_hint(const char *text);

static void open_book(int list_index)
{
    int book_idx = list_index;
    if (book_idx < 0 || book_idx >= s_state.book_count) return;
    reader_book_info_t *b = &s_state.books[book_idx];
    snprintf(s_state.current_id, sizeof(s_state.current_id), "%s", b->id);
    snprintf(s_state.current_title, sizeof(s_state.current_title), "%s", b->title);
    // Progress is measured in ORIGINAL UTF-8 byte offsets (bookmarks and
    // chapter offsets use the same space), so progress metrics must use the
    // uncompressed length. The title card, however, shows the real on-flash
    // footprint of the compressed book — keep the two values separate.
    s_state.book_size = b->orig_size ? b->orig_size : b->size;
    s_state.book_flash_size = b->size;
    s_state.book_chars = 0;
    s_state.selected = list_index;
    history_reset();
    s_unpersisted_pages = 0;

    // A book with no saved progress has never been opened. Put its title on
    // screen FIRST: building the chapter index and the back-history walks the
    // whole file, which is the noticeable pause on a long novel. The cover is
    // rendered before that work starts, so the wait happens behind a page the
    // reader can actually look at, and the cover stays up until a key arrives.
    const bool first_open = !reader_store_has_progress(s_state.current_id);
    if (first_open) {
        s_state.screen = READER_SCREEN_COVER;
        render_screen();
    }

    atomic_store(&s_loading, true);
    s_loading_started = now_ms();
    s_loading_updated = s_loading_started;
    uint16_t saved_chapter = 0;
    s_state.page_offset = reader_store_get_offset(s_state.current_id, &saved_chapter);
    if (s_state.page_offset > s_state.book_size) s_state.page_offset = 0;
    if (reader_store_build_index(s_state.current_id, &s_state.index,
                                index_loading_progress, NULL) != ESP_OK) {
        s_state.current_id[0] = '\0';
        s_state.screen = READER_SCREEN_LIBRARY;
        set_hint("书籍加载失败");
        atomic_store(&s_loading, false);
        return;
    }
    s_state.book_chars = s_state.index.char_count;
    s_state.toc_selected = saved_chapter < s_state.index.count ? saved_chapter : 0;
    s_state.chapter = (int)s_state.toc_selected;
    prepare_page();
    // Rebuild the back history up to the resumed page so the first UP press
    // turns back instead of being swallowed by an empty stack.
    s_history_count = reader_store_history_load(s_state.current_id, s_state.page_offset,
                                                history_layout_key(), s_history);
    if (!s_history_count) {
        rebuild_history_walk(chapter_start_offset(s_state.chapter), s_state.page_offset, true);
        save_resume_history();
    }
    // Page numbers exist only under a geometry: count them in the same loading
    // pass as the index, so the footer and the TOC page tab are ready the moment
    // the page appears.
    pagemap_rebuild("正在统计页数");
    // Every session starts manual: the key toggles auto page-turning on demand.
    s_auto_mode = 0;
    s_auto_elapsed_ms = 0;
    s_state.screen = first_open ? READER_SCREEN_COVER : READER_SCREEN_READER;
    note_activity();
    atomic_store(&s_loading, false);
}

static void close_book(void)
{
    if (s_state.current_id[0]) {
        persist_progress();
        save_resume_history();
    }
    s_state.current_id[0] = '\0';
    s_auto_mode = 0;
    // Pages are a property of an open book: never let the shelf or a later book
    // display the previous one's count.
    reader_pagemap_init(&s_state.pagemap, s_state.pagemap_anchors,
                        s_state.pagemap_anchors ? PAGEMAP_ANCHOR_BUDGET : 0);
    s_state.page_no = 0;
    s_state.screen = READER_SCREEN_LIBRARY;
    refresh_book_list();
}

static void page_delta(int delta, int persist)
{
    if (delta > 0) {
        if (s_state.next_offset <= s_state.page_offset ||
            s_state.next_offset >= s_state.book_size) {
            s_auto_mode = 0;  // stop auto-advance at the end of the book
            return;
        }
        history_push(s_state.page_offset);
        s_state.page_offset = s_state.next_offset;
    } else {
        uint32_t prev;
        if (!history_pop(&prev)) {
            // First page of a chapter (fresh open or a TOC jump): step onto the
            // previous chapter's last page. Books without chapter headings fall
            // back to a window search so UP never silently does nothing.
            if (!prev_chapter_last_page(&prev) && !prev_page_in_window(&prev)) {
                // Page 0 has no predecessor: step back onto the title card
                // instead of swallowing the key. The cover is therefore a
                // real, flippable page 0, not only a first-open splash.
                if (s_state.page_offset == 0) {
                    s_state.screen = READER_SCREEN_COVER;
                }
                return;
            }
        }
        s_state.page_offset = prev;
    }
    prepare_page();
    note_activity();
    if (persist && ++s_unpersisted_pages >= PERSIST_EVERY) {
        persist_progress();
        save_resume_history();
    }
}

static void goto_chapter(int index)
{
    if (index < 0 || index >= s_state.index.count) return;
    s_state.page_offset = (uint32_t)s_state.index.items[index].offset;
    s_state.chapter = index;
    s_state.toc_selected = index;
    history_reset();
    prepare_page();
    persist_progress();
    s_state.screen = READER_SCREEN_READER;
}

// Jump to the first page of a 10-page range picked in the TOC page tab. This
// exists because chapter detection is heuristic: when a book's headings are
// missing or wrong, a page range is still a dependable place to land.
static void goto_page_group(int group)
{
    if (group < 0 || (uint32_t)group >= s_state.pagemap.groups) return;
    const uint32_t page = (uint32_t)group * READER_PAGEMAP_GROUP + 1;
    uint32_t offset = 0;
    if (!reader_pagemap_offset_of(&s_state.pagemap, page_next_cb, NULL, page, &offset)) return;
    if (offset >= s_state.book_size) return;

    s_state.page_offset = offset;
    // Land in the chapter that owns this page so the header subtitle is right.
    int best = 0;
    for (int i = 0; i < s_state.index.count; ++i) {
        if (s_state.index.items[i].offset <= offset) best = i;
    }
    s_state.chapter = best;
    s_state.toc_selected = s_state.chapter;
    prepare_page();
    rebuild_history(chapter_start_offset(s_state.chapter), offset);
    persist_progress();
    s_state.screen = READER_SCREEN_READER;
}

static void set_hint(const char *text)
{
    snprintf(s_hint, sizeof(s_hint), "%s", text ? text : "");
    s_hint_until_ms = now_ms() + HINT_DURATION_MS;
}

static void load_bookmarks(void)
{
    s_state.bookmark_count = s_state.current_id[0]
        ? reader_store_bookmark_list(reader_book_id(s_state.current_id),
                                     s_state.bookmarks, READER_BOOKMARK_MAX)
        : 0;
    for (int i = 0; i < READER_BOOKMARK_MAX; ++i) {
        s_state.bookmark_titles[i][0] = '\0';
        s_state.bookmark_title_ptrs[i] = s_state.bookmark_titles[i];
    }
    for (int i = 0; i < s_state.bookmark_count; ++i) {
        int n = reader_store_bookmark_excerpt(s_state.current_id,
                                              s_state.bookmarks[i].offset,
                                              s_state.bookmark_titles[i],
                                              READER_BOOKMARK_TITLE_SIZE);
        if (n <= 0) {
            snprintf(s_state.bookmark_titles[i], READER_BOOKMARK_TITLE_SIZE,
                     "%s", "书签位置");
        }
    }
    if (s_state.toc_selected >= s_state.bookmark_count) {
        s_state.toc_selected = s_state.bookmark_count > 0
                                   ? s_state.bookmark_count - 1 : 0;
    }
}

static void goto_bookmark(int index)
{
    if (index < 0 || index >= s_state.bookmark_count) return;
    // A bookmark is an ABSOLUTE BYTE OFFSET, never a page number. Immersive
    // mode changes how many lines fit on a page, so a page index would land
    // somewhere else after a mode switch. Starting pagination at this exact
    // byte always shows the same sentence, under either geometry.
    s_state.page_offset = s_state.bookmarks[index].offset;
    s_state.chapter = (int)s_state.bookmarks[index].chapter;
    s_state.toc_selected = s_state.chapter;
    prepare_page();
    // Rebuild the back history with the CURRENT line budget. If the saved
    // offset is not a page boundary under this geometry, rebuild_history()
    // clears the stack and the window-search fallback takes over.
    rebuild_history(chapter_start_offset(s_state.chapter), s_state.page_offset);
    persist_progress();
    s_state.screen = READER_SCREEN_READER;
}

static void toggle_bookmark(void)
{
    if (!s_state.current_id[0]) return;
    // Record the byte offset of the visible page start; see goto_bookmark().
    int added = reader_store_bookmark_toggle(reader_book_id(s_state.current_id),
                                             s_state.page_offset,
                                             (uint16_t)s_state.chapter);
    if (added < 0) return;
    load_bookmarks();
    set_hint(added ? "已添加书签" : "已移除书签");
}

// A long UP/DOWN never deletes anything by itself: it snapshots the target and
// opens the confirmation page. The snapshot (book id or bookmark offset) is
// what confirm_delete() acts on, so stray keys while the prompt is up cannot
// retarget the deletion. Both callers clear any stale footer hint first.
static void request_delete_bookmark(void)
{
    if (!s_state.current_id[0] || s_state.bookmark_count <= 0) {
        set_hint("还没有书签");
        return;
    }
    int sel = s_state.toc_selected;
    if (sel < 0) sel = 0;
    if (sel >= s_state.bookmark_count) sel = s_state.bookmark_count - 1;
    s_hint[0] = '\0';
    s_state.confirm_kind = 1;
    s_state.confirm_from = READER_SCREEN_TOC;
    s_state.confirm_offset = s_state.bookmarks[sel].offset;
    s_state.confirm_id[0] = '\0';
    snprintf(s_state.confirm_label, sizeof(s_state.confirm_label), "%s",
             s_state.bookmark_titles[sel][0] ? s_state.bookmark_titles[sel]
                                             : "该书签");
    s_state.screen = READER_SCREEN_CONFIRM;
}

// Row `book_count` (settings) is an action, not a book, and the guide is the
// only protected built-in. Everything else asks for confirmation.
static void request_delete_book(void)
{
    int book_idx = s_state.selected;
    if (book_idx < 0 || book_idx >= s_state.book_count) {
        set_hint("此项不可删除");
        return;
    }
    const char *id = s_state.books[book_idx].id;
    if (strcmp(id, READER_BUILTIN_GUIDE_ID) == 0) {
        set_hint("使用指南不可删除");
        return;
    }
    s_hint[0] = '\0';
    s_state.confirm_kind = 2;
    s_state.confirm_from = READER_SCREEN_LIBRARY;
    s_state.confirm_offset = 0;
    snprintf(s_state.confirm_id, sizeof(s_state.confirm_id), "%s", id);
    snprintf(s_state.confirm_label, sizeof(s_state.confirm_label), "%s",
             s_state.books[book_idx].title);
    s_state.screen = READER_SCREEN_CONFIRM;
}

// The second, explicit OK press on the confirmation page. The store purges the
// deleted book's progress/bookmarks too, so a re-upload starts clean. Load the
// bookmark list again after a removal so the remaining labels stay aligned.
static void confirm_delete(void)
{
    int from = s_state.confirm_from;
    int kind = s_state.confirm_kind;
    s_state.confirm_kind = 0;
    s_state.screen = from;

    if (kind == 2) {
        esp_err_t err = reader_store_delete_book(s_state.confirm_id);
        if (err == ESP_ERR_NOT_SUPPORTED) set_hint("使用指南不可删除");
        else if (err == ESP_OK) set_hint("已删除本书");
        else set_hint("删除失败");
        if (err == ESP_OK) {
            refresh_book_list();
            if (s_state.selected >= s_state.book_count + 1) s_state.selected = 0;
        }
    } else if (kind == 1) {
        int removed = reader_store_bookmark_remove(
            reader_book_id(s_state.current_id), s_state.confirm_offset);
        set_hint(removed ? "已删除书签" : "删除失败");
        load_bookmarks();
    }
}

static void cancel_delete(void)
{
    s_state.confirm_kind = 0;
    s_state.screen = s_state.confirm_from;
}

static void enter_transfer(void)
{
    esp_err_t err = reader_transfer_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败: %s", esp_err_to_name(err));
    }
    // 接收新书 now lives inside settings, so leaving the hotspot must land back
    // on the settings page, not the shelf. Keep the source for any future entry.
    s_state.transfer_from = s_state.screen;
    s_state.screen = READER_SCREEN_TRANSFER;
}

static void leave_transfer(void)
{
    reader_transfer_stop();
    s_state.screen = s_state.transfer_from;
    if (s_state.screen == READER_SCREEN_LIBRARY) refresh_book_list();
}

// Repaginate at the EXACT byte offset already on screen. Immersive mode only
// changes how many lines fit below the top sentence; keeping the offset means
// that sentence does not move at all, and bookmarks continue to resolve to the
// text the reader was looking at.
static void repaginate_in_place(void)
{
    if (!s_state.current_id[0]) return;
    prepare_page();
    rebuild_history(chapter_start_offset(s_state.chapter), s_state.page_offset);
    persist_progress();
}

// Walk page starts from `start` with the CURRENT geometry and return the last
// boundary at or before `target`. Kept in its own frame: the caller also needs
// prepare_page() and rebuild_history(), each of which owns another ~1-2 KB of
// read/pagination buffers. Nesting them would overflow the input task's stack.
static uint32_t page_start_at_or_before(uint32_t start, uint32_t target)
{
    char text[PAGE_WINDOW_SIZE];
    char out[PAGE_TEXT_SIZE];
    uint32_t cur = start;
    uint32_t best = start;
    // A chapter longer than this is pathological; stopping keeps the key press
    // responsive and merely lands the reader at the last boundary found.
    for (int guard = 0; guard < 512; ++guard) {
        int n = reader_store_read(s_state.current_id, cur, text, sizeof(text));
        if (n <= 0) break;
        reader_page_t page;
        if (paginate(text, (size_t)n, 0,
                            out, sizeof(out), &page) != 0) break;
        if (page.next == 0) break;
        uint32_t next = cur + (uint32_t)page.next;
        if (next > target) break;
        best = next;
        cur = next;
    }
    return best;
}

// Re-flow after a FONT SIZE change: a different wrap width moves every line
// boundary, so walk page starts from the chapter start with the new geometry
// and land on the last one that does not pass the sentence being read.
static void reflow_current_page(void)
{
    if (!s_state.current_id[0]) return;

    uint32_t start = chapter_start_offset(s_state.chapter);
    if (s_state.page_offset < start) start = 0;
    const uint32_t target = s_state.page_offset;
    uint32_t best = page_start_at_or_before(start, target);

    // Sequential, not nested: each helper's big buffers are gone before the
    // next one starts, so the peak stack stays a single ~2 KB window.
    s_state.page_offset = best;
    prepare_page();
    rebuild_history(start, best);
    persist_progress();
}

static void commit_font_size(void)
{
    reader_store_set_setting("font", (uint8_t)s_state.font_size);
    if (!s_state.current_id[0]) return;

    // Every page boundary moves, so both the current page and the book's page
    // count have to be recomputed. On a multi-megabyte book that is seconds of
    // work: show the same progress overlay the first open uses instead of
    // leaving a frozen screen behind.
    begin_progress("正在重排");
    reflow_current_page();
    pagemap_rebuild("正在重排");
}

// Persist the ruler pages on the way out instead of on every step, so scrubbing
// across eleven brightness values is one NVS write, not eleven.
static void commit_ruler_setting(void)
{
    reader_store_set_setting("bl", (uint8_t)s_state.backlight);
    reader_store_set_setting("auto", (uint8_t)s_state.auto_seconds);
    // A new interval restarts the countdown rather than inheriting the old one.
    s_auto_elapsed_ms = 0;
}

static void cycle_setting(int row)
{
    switch (row) {
    case 0:
        // 接收新书 is pinned at the top of settings: start the hotspot.
        enter_transfer();
        break;
    case 1:
        // The font picker lets the user choose any size between 12 and 20
        // while watching a live preview of the reading page.
        s_state.screen = READER_SCREEN_FONT_PICKER;
        break;
    case 2:
        // Brightness gets its own ruler page, exactly like the font size: the
        // old "cycle in place" row made picking 40 % a four-press job with no
        // feedback until you left the settings.
        s_state.backlight = (int)s_state.backlight;
        s_state.screen = READER_SCREEN_BACKLIGHT;
        break;
    case 3:
        // Auto page-turn interval. Its page also explains the reading-screen
        // shortcut, because that is where a reader would look for it.
        s_state.screen = READER_SCREEN_AUTOPAGE;
        break;
    case 4:
        s_state.idle_seconds = s_state.idle_seconds == 30 ? 60 :
                               s_state.idle_seconds == 60 ? 120 : 30;
        reader_store_set_setting("idle", (uint8_t)s_state.idle_seconds);
        break;
    case 5:
        s_state.dark = !s_state.dark;
        reader_store_set_setting("dark", (uint8_t)s_state.dark);
        break;
    case 6:
        s_state.immersive = !s_state.immersive;
        reader_store_set_setting("immerse", (uint8_t)s_state.immersive);
        // The line budget just changed, so the text currently in page_text was
        // paginated with the old geometry. Repaginate at the same byte offset:
        // the top sentence does not move, the page just shows more (or fewer)
        // lines, and a bookmark taken afterwards points at the visible text.
        repaginate_in_place();
        if (s_state.current_id[0]) {
            begin_progress("正在重排");
            pagemap_rebuild("正在重排");
        }
        break;
    case 7:
        // Storage is a read-only sub-page; it queries the filesystem when rendered.
        s_state.screen = READER_SCREEN_STORAGE;
        break;
    default:
        break;
    }
}

// Where a TOC list must start when its tab is entered. The point of the tab row
// is "take me to where I am", so every tab opens on the reader's own position
// instead of row 0: the chapter being read, the nearest bookmark at or before
// this page, or the 10-page range that contains it. Starting at 0 forced a
// scroll from the beginning of the book every single time.
static void toc_select_current(void)
{
    switch (s_state.toc_mode) {
    case 0:
        s_state.toc_selected = s_state.chapter;
        if (s_state.toc_selected >= s_state.index.count) {
            s_state.toc_selected = s_state.index.count > 0 ? s_state.index.count - 1 : 0;
        }
        if (s_state.toc_selected < 0) s_state.toc_selected = 0;
        break;
    case 1: {
        // Bookmarks are byte offsets, so "nearest" is the greatest offset at or
        // before the current page. No earlier bookmark means the first one.
        int best = -1;
        uint32_t best_offset = 0;
        for (int i = 0; i < s_state.bookmark_count; ++i) {
            const uint32_t offset = (uint32_t)s_state.bookmarks[i].offset;
            if (offset <= s_state.page_offset &&
                (best < 0 || offset >= best_offset)) {
                best = i;
                best_offset = offset;
            }
        }
        if (best < 0 && s_state.bookmark_count > 0) {
            // All bookmarks are ahead of the reader: start at the closest one.
            best = 0;
            for (int i = 1; i < s_state.bookmark_count; ++i) {
                if ((uint32_t)s_state.bookmarks[i].offset <
                    (uint32_t)s_state.bookmarks[best].offset) {
                    best = i;
                }
            }
        }
        s_state.toc_selected = best >= 0 ? best : 0;
        break;
    }
    default: {
        uint32_t group = 0;
        if (s_state.page_no > 0) {
            group = (s_state.page_no - 1) / READER_PAGEMAP_GROUP;
        }
        const uint32_t groups = s_state.pagemap.groups;
        if (groups > 0 && group >= groups) group = groups - 1;
        s_state.toc_selected = groups > 0 ? (int)group : 0;
        break;
    }
    }
}

// Begin hold-to-repeat on a ruler or a TOC list: one immediate step, then
// SCROLL_PERIOD_MS repeats until the key is released.
static void scroll_begin(int delta);

static void move_selection(int delta)
{
    int max = 0;
    switch (s_state.screen) {
    case READER_SCREEN_LIBRARY: max = s_state.book_count + 1; break;
    case READER_SCREEN_TOC:
        // Mode 2 lists page ranges, so its length is the number of 10-page
        // groups the page map produced, not a chapter/bookmark count.
        max = s_state.toc_mode == 0 ? s_state.index.count
            : s_state.toc_mode == 1 ? s_state.bookmark_count
                                    : (int)s_state.pagemap.groups;
        break;
    case READER_SCREEN_SETTINGS: max = 8; break;
    default: return;
    }
    int *sel = s_state.screen == READER_SCREEN_LIBRARY ? &s_state.selected :
               s_state.screen == READER_SCREEN_TOC ? &s_state.toc_selected :
               &s_state.setting_selected;
    // An empty list (e.g. no bookmarks yet) must not leave the cursor at -1.
    if (max <= 0) { *sel = 0; return; }
    int next = *sel + delta;
    // Wrap at both ends: UP on the first item jumps to the last, DOWN on the
    // last jumps back to the first. Only list cursors wrap; the font-size
    // ruler still stops at 12 and 20 because those are hard limits, not rows.
    if (next < 0) next = max - 1;
    else if (next >= max) next = 0;
    *sel = next;
}

// Every ruler wraps at both ends, so holding a direction never dead-ends.
static int wrap_index(int index, int count)
{
    if (count <= 0) return 0;
    index %= count;
    if (index < 0) index += count;
    return index;
}

// UP grows the body text, DOWN shrinks it, cycling through the selectable sizes.
// Step by TABLE POSITION, not by pixel: 17 and 19 px are not selectable, so
// `size + 1` would ask for a font that is not compiled in.
static void step_font_size(int delta)
{
    int index = wrap_index(reader_layout_font_index(s_state.font_size) + delta,
                           READER_FONT_SIZE_COUNT);
    s_state.font_size = reader_layout_font_size(index);
}

// One step of whatever the three pickers and the TOC lists show. Short taps and
// the held-key timer both go through here, so holding a key repeats exactly what
// a single tap does — no second, subtly different fast path.
static void scroll_step(int delta)
{
    switch (s_state.screen) {
    case READER_SCREEN_TOC:
        if (s_state.toc_focus != 1) {
            s_scroll_dir = 0;
            return;
        }
        move_selection(delta);
        return;
    case READER_SCREEN_FONT_PICKER:
        step_font_size(delta);
        return;
    case READER_SCREEN_BACKLIGHT:
        s_state.backlight = reader_backlight_step_value(
            wrap_index(reader_backlight_step_index(s_state.backlight) + delta,
                       READER_BACKLIGHT_STEPS));
        bsp_display_backlight((uint8_t)s_state.backlight);
        s_dim_stage = 0;
        return;
    case READER_SCREEN_AUTOPAGE:
        s_state.auto_seconds = reader_autopage_option(
            wrap_index(reader_autopage_index(s_state.auto_seconds) + delta,
                       READER_AUTOPAGE_OPTIONS));
        return;
    default:
        s_scroll_dir = 0;
        return;
    }
}

static void scroll_begin(int delta)
{
    if (!delta) return;
    s_scroll_dir = delta > 0 ? 1 : -1;
    scroll_step(s_scroll_dir);
}

// Runs inside the LVGL task, so it may draw directly. Any release stops it, and
// scroll_step() clears the direction if the screen moved on underneath.
static void scroll_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_scroll_dir == 0) return;
    scroll_step(s_scroll_dir);
    render_screen_locked();
}

// Toggle auto page-turning from the reading screen. Bound to a double tap for
// now: the hardware "power" key is not wired to the MCU (bsp_pins.h has only the
// three ADC keys), and every single-tap gesture on this screen is already taken
// by paging / 目录 / 书签 / 设置 / 退出. A double tap is free because iot_button
// emits BUTTON_SINGLE_CLICK only after the double-click window expires, so a
// double tap never reaches the paging branch first.
static void toggle_auto_page(void)
{
    s_auto_mode = s_auto_mode ? 0 : 1;
    s_auto_elapsed_ms = 0;
    note_activity();
    set_hint(s_auto_mode ? "自动翻页 开" : "自动翻页 关");
}

static bool is_short(int event)
{
    // iot_button merges two quick taps into a single DOUBLE event; treating it
    // as a normal click keeps rapid page turning responsive.
    return event == BSP_BTN_CLICK || event == BSP_BTN_DOUBLE;
}

static void process_key(const input_event_t *in)
{
    note_activity();
    int64_t now_us = esp_timer_get_time();
    if (in->event == BSP_BTN_RELEASE) {
        s_scroll_dir = 0;  // the held key is up: stop the fast scroll
        return;
    }
    if (in->event == BSP_BTN_PRESS) {
        return;  // wake only
    }
    // Any new key event ends a previous hold before it is interpreted, so a
    // stale direction can never keep moving the list behind the user's back.
    s_scroll_dir = 0;

    const bool is_long = in->event == BSP_BTN_LONG;
    if (!is_long && now_us < s_ignore_until_us) return;  // suppress post-long click

    if (is_long) {
        // A long press can be followed by a spurious CLICK; swallow it briefly.
        s_ignore_until_us = now_us + LONG_IGNORE_MS * 1000;
        switch (s_state.screen) {
        case READER_SCREEN_READER:
            // Long-press DOWN opens settings without leaving the book, UP
            // toggles a bookmark, and OK returns to the shelf.
            if (in->btn == BSP_BTN_DOWN) {
                persist_progress();
                s_state.setting_selected = 0;
                s_state.settings_from_reader = 1;
                s_state.screen = READER_SCREEN_SETTINGS;
            } else if (in->btn == BSP_BTN_UP) {
                toggle_bookmark();
            } else if (in->btn == BSP_BTN_OK) {
                close_book();
            }
            break;
        case READER_SCREEN_LIBRARY:
            // UP/DOWN opens the delete confirmation; OK is "return". The shelf
            // is the root screen, so OK-long resets the cursor instead of
            // navigating to a parent that does not exist.
            if (in->btn == BSP_BTN_OK) {
                s_state.selected = 0;
                refresh_book_list();
            } else {
                request_delete_book();
            }
            break;
        case READER_SCREEN_TOC:
            if (in->btn == BSP_BTN_OK) {
                // Long OK is "back", one level per press: the list returns to the
                // tab row, and the tab row leaves the TOC for the reading page.
                if (s_state.toc_focus == 1) {
                    s_state.toc_focus = 0;
                } else {
                    s_state.screen = READER_SCREEN_READER;
                }
            } else if (s_state.toc_mode == 1 && s_state.toc_focus == 1) {
                // The bookmark list keeps its delete gesture, and a handful of
                // bookmarks never needs fast scrolling.
                request_delete_bookmark();
            } else if (s_state.toc_focus == 1) {
                // Chapter and page-range lists: hold UP/DOWN to scroll fast.
                scroll_begin(in->btn == BSP_BTN_UP ? -1 : 1);
            } else {
                s_state.screen = READER_SCREEN_READER;
            }
            break;
        case READER_SCREEN_STORAGE:
            // Back to settings; settings still remembers whether it was opened
            // from the reader, so a second long OK resumes reading.
            s_state.screen = READER_SCREEN_SETTINGS;
            break;
        case READER_SCREEN_SETTINGS:
            // Return where the user came from: settings opened from the reader
            // page must not drop them back to the shelf mid-book.
            if (s_state.settings_from_reader && s_state.current_id[0]) {
                s_state.settings_from_reader = 0;
                s_state.screen = READER_SCREEN_READER;
            } else {
                s_state.screen = READER_SCREEN_LIBRARY;
                refresh_book_list();
            }
            break;
        case READER_SCREEN_FONT_PICKER:
            if (in->btn == BSP_BTN_OK) {
                // Leaving the picker is the commit point: re-flow once instead
                // of on every key press, which keeps the preview instant.
                commit_font_size();
                s_state.screen = READER_SCREEN_SETTINGS;
            } else {
                scroll_begin(in->btn == BSP_BTN_UP ? 1 : -1);
            }
            break;
        case READER_SCREEN_BACKLIGHT:
        case READER_SCREEN_AUTOPAGE:
            if (in->btn == BSP_BTN_OK) {
                commit_ruler_setting();
                s_state.screen = READER_SCREEN_SETTINGS;
            } else {
                // Hold UP/DOWN to sweep the ladder: same repeat as the TOC page
                // lists, and it wraps, so one long press reaches any value.
                scroll_begin(in->btn == BSP_BTN_UP ? 1 : -1);
            }
            break;
        case READER_SCREEN_FINISHED:
            // Only OK-long acts as the advertised "OK" exit. Holding UP/DOWN
            // must not add a bookmark or open settings on a confirmation page.
            if (in->btn == BSP_BTN_OK) close_book();
            break;
        case READER_SCREEN_COVER:
            // Long OK backs out without starting to read; the other long
            // presses must not add a bookmark or open settings here.
            if (in->btn == BSP_BTN_OK) close_book();
            break;
        case READER_SCREEN_TRANSFER:
            leave_transfer();
            break;
        case READER_SCREEN_CONFIRM:
            // Any long press is an unambiguous "cancel": go back without
            // touching the store, exactly like a short UP/DOWN.
            cancel_delete();
            break;
        default:
            break;
        }
        render_screen();
        return;
    }
    // Auto page-turn toggle, checked before the click branches: see
    // toggle_auto_page() for why a double tap is the available gesture.
    if (in->event == BSP_BTN_DOUBLE && in->btn == BSP_BTN_OK &&
        s_state.screen == READER_SCREEN_READER) {
        toggle_auto_page();
        render_screen();
        return;
    }
    if (!is_short(in->event)) return;

    const int btn = in->btn;
    if (btn == BSP_BTN_UP) {
        switch (s_state.screen) {
        case READER_SCREEN_LIBRARY:
        case READER_SCREEN_SETTINGS:
            move_selection(-1);
            break;
        case READER_SCREEN_TOC:
            if (s_state.toc_focus == 0) {
                // Tab row: UP moves LEFT, DOWN moves RIGHT (wrapping at both
                // ends), matching the left-to-right order of the three tabs.
                s_state.toc_mode = (s_state.toc_mode + 2) % 3;
                toc_select_current();
            } else {
                move_selection(-1);
            }
            break;
        case READER_SCREEN_READER:
            page_delta(-1, 1);
            break;
        case READER_SCREEN_FONT_PICKER:
            step_font_size(1);
            break;
        case READER_SCREEN_BACKLIGHT:
        case READER_SCREEN_AUTOPAGE:
            scroll_step(1);
            break;
        case READER_SCREEN_FINISHED:
            // UP steps back to the last page rather than leaving the book: the
            // confirmation page advertises DOWN/OK as its only exits.
            s_state.screen = READER_SCREEN_READER;
            break;
        case READER_SCREEN_COVER:
            s_state.screen = READER_SCREEN_READER;
            break;
        case READER_SCREEN_CONFIRM:
            cancel_delete();
            break;
        default:
            break;
        }
    } else if (btn == BSP_BTN_DOWN) {
        switch (s_state.screen) {
        case READER_SCREEN_LIBRARY:
        case READER_SCREEN_SETTINGS:
            move_selection(1);
            break;
        case READER_SCREEN_TOC:
            if (s_state.toc_focus == 0) {
                // Tab row: UP/DOWN cycles chapters -> bookmarks -> pages.
                s_state.toc_mode = (s_state.toc_mode + 1) % 3;
                toc_select_current();
            } else {
                move_selection(1);
            }
            break;
        case READER_SCREEN_READER:
            if (at_book_end()) {
                persist_progress();
                s_auto_mode = 0;
                s_state.screen = READER_SCREEN_FINISHED;
            } else {
                page_delta(1, 1);
            }
            break;
        case READER_SCREEN_FONT_PICKER:
            step_font_size(-1);
            break;
        case READER_SCREEN_BACKLIGHT:
        case READER_SCREEN_AUTOPAGE:
            scroll_step(-1);
            break;
        case READER_SCREEN_COVER:
            s_state.screen = READER_SCREEN_READER;
            break;
        case READER_SCREEN_FINISHED:
            close_book();
            break;
        case READER_SCREEN_CONFIRM:
            cancel_delete();
            break;
        default:
            break;
        }
    } else if (btn == BSP_BTN_OK) {
        switch (s_state.screen) {
        case READER_SCREEN_LIBRARY: {
            int sel = s_state.selected;
            if (sel == s_state.book_count) {
                // Pinned bottom row: settings, which now owns 接收新书.
                s_state.setting_selected = 0;
                s_state.settings_from_reader = 0;
                s_state.screen = READER_SCREEN_SETTINGS;
            } else if (sel >= 0 && sel < s_state.book_count) {
                open_book(sel);
            }
            break;
        }
        case READER_SCREEN_READER:
            persist_progress();
            load_bookmarks();
            s_state.toc_mode = 0;
            s_state.toc_focus = 0;
            s_state.toc_selected = s_state.chapter;
            s_state.screen = READER_SCREEN_TOC;
            break;
        case READER_SCREEN_TOC:
            if (s_state.toc_focus == 0) {
                s_state.toc_focus = 1;
                toc_select_current();
            } else if (s_state.toc_mode == 0) {
                goto_chapter(s_state.toc_selected);
            } else if (s_state.toc_mode == 1) {
                goto_bookmark(s_state.toc_selected);
            } else {
                goto_page_group(s_state.toc_selected);
            }
            break;
        case READER_SCREEN_SETTINGS:
            cycle_setting(s_state.setting_selected);
            break;
        case READER_SCREEN_FONT_PICKER:
            // A tap on OK saves and returns, same as a long press: the two
            // would otherwise have to be explained in a one-line hint.
            commit_font_size();
            s_state.screen = READER_SCREEN_SETTINGS;
            break;
        case READER_SCREEN_BACKLIGHT:
        case READER_SCREEN_AUTOPAGE:
            commit_ruler_setting();
            s_state.screen = READER_SCREEN_SETTINGS;
            break;
        case READER_SCREEN_TRANSFER:
            // Deliberately do nothing: only OK-long stops the AP, so an
            // accidental tap cannot interrupt an upload.
            break;
        case READER_SCREEN_COVER:
            s_state.screen = READER_SCREEN_READER;
            break;
        case READER_SCREEN_FINISHED:
            close_book();
            break;
        case READER_SCREEN_CONFIRM:
            // The one key that actually mutates the store; everything else on
            // this page is a cancellation.
            confirm_delete();
            break;
        default:
            break;
        }
    }
    render_screen();
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t in;
    for (;;) {
        if (xQueueReceive(s_queue, &in, portMAX_DELAY) == pdTRUE && s_ready) {
            process_key(&in);
        }
    }
}

void reader_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Runs on the button component's shared esp_timer task: enqueue only.
    if (!s_queue || !s_ready || atomic_load(&s_loading)) return;
    input_event_t in = { .btn = (int)btn, .event = (int)ev };
    (void)xQueueSend(s_queue, &in, 0);
}

// Runs inside the LVGL task (already holding the port lock).
static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (atomic_load(&s_loading)) return;

    // Retire the transient footer hint once its two seconds are up. The shelf,
    // TOC and reader all show the hint in place of their normal key chips, so
    // every one of them must be rebuilt or the pill would stay on screen.
    if (s_hint[0] && (int32_t)(now_ms() - s_hint_until_ms) >= 0) {
        s_hint[0] = '\0';
        if (s_state.screen == READER_SCREEN_READER ||
            s_state.screen == READER_SCREEN_TOC ||
            s_state.screen == READER_SCREEN_LIBRARY) {
            render_screen_locked();
        }
    }

    s_battery_tick++;
    if (s_battery_tick >= BATTERY_TICKS) {
        s_battery_tick = 0;
        reader_ui_set_battery(battery_text());
    }

    if (s_state.screen == READER_SCREEN_TRANSFER) {
        reader_transfer_status_t status;
        size_t total = 0, used = 0, free_b = 0;
        reader_transfer_get_status(&status);
        space_snapshot(&total, &used, &free_b);
        reader_ui_refresh_transfer(&status, total, used, free_b);
        s_last_activity_ms = now_ms();  // keep the transfer page readable
        return;
    }

    // Auto page advance counts as activity, so the screen stays on. The interval
    // comes from the 自动翻页 setting; a zero interval means the reader turned it
    // off, and the timer must not keep a stale mode alive.
    const uint32_t auto_ms = (uint32_t)s_state.auto_seconds * 1000u;
    if (s_auto_mode && s_state.screen == READER_SCREEN_READER) {
        s_auto_elapsed_ms += TICK_PERIOD_MS;
        if (s_auto_elapsed_ms >= auto_ms) {
            s_auto_elapsed_ms = 0;
            page_delta(1, 0);
            if (!s_auto_mode) persist_progress();
            render_screen_locked();
        }
    }

    // Idle dims, then blanks the backlight; any key restores it.
    uint32_t idle_ms = now_ms() - s_last_activity_ms;
    uint32_t limit_ms = (uint32_t)s_state.idle_seconds * 1000u;
    if (idle_ms > limit_ms * 2u && s_dim_stage != 2) {
        bsp_display_backlight(0);
        s_dim_stage = 2;
    } else if (idle_ms > limit_ms && idle_ms <= limit_ms * 2u && s_dim_stage != 1) {
        bsp_display_backlight(15);
        s_dim_stage = 1;
    }
}

esp_err_t reader_app_init(bsp_btn_cb_t key_cb)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.screen = READER_SCREEN_LIBRARY;
    s_state.font_size = reader_store_get_setting("font", READER_FONT_DEFAULT_SIZE);
    // A value written by an older firmware (or a corrupt NVS) must not become
    // an out-of-range font request.
    if (!reader_layout_size_valid(s_state.font_size)) {
        s_state.font_size = READER_FONT_DEFAULT_SIZE;
    }
    // Both ladders live in reader_ui (it draws them), so a value written by an
    // older firmware snaps onto the current ladder instead of being rejected.
    s_state.backlight = reader_backlight_step_value(
        reader_backlight_step_index(reader_store_get_setting("bl", 100)));
    s_state.auto_seconds = reader_autopage_option(
        reader_autopage_index(reader_store_get_setting("auto", AUTO_PAGE_DEFAULT)));
    s_state.idle_seconds = reader_store_get_setting("idle", 60);
    if (s_state.idle_seconds != 30 && s_state.idle_seconds != 60 &&
        s_state.idle_seconds != 120) {
        s_state.idle_seconds = 60;
    }
    s_state.dark = reader_store_get_setting("dark", 0) ? 1 : 0;
    s_state.immersive = reader_store_get_setting("immerse", 0) ? 1 : 0;
    refresh_book_list();

    s_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (!bsp_lvgl_lock(1000)) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_TIMEOUT;
    }
    reader_ui_init();
    s_tick_timer = lv_timer_create(tick, TICK_PERIOD_MS, NULL);
    s_scroll_timer = lv_timer_create(scroll_tick, SCROLL_PERIOD_MS, NULL);
    render_screen_locked();
    bsp_lvgl_unlock();

    // Worst measured frame chain is one ~2.3 KB pagination window plus LVGL
    // rendering on top. 6 KiB leaves real headroom without touching the heap
    // budget: the two nested-window stack overflows that used to panic the
    // device are fixed above, this is defence in depth.
    if (xTaskCreate(input_task, "reader_input", 6144, NULL, 5, &s_task) != pdPASS) {
        reader_app_deinit();
        return ESP_ERR_NO_MEM;
    }
    // main.c owns the callback so the deep-sleep key-wake guard can consume a
    // release event before input dispatch is declared ready.
    esp_err_t err = bsp_button_init(key_cb ? key_cb : reader_app_on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
        reader_app_deinit();
        return err;
    }

    bsp_display_backlight((uint8_t)s_state.backlight);
    s_ready = true;
    s_last_activity_ms = now_ms();
    ESP_LOGI(TAG, "阅读器就绪：%d 本书，字号 %d px，亮度 %d%%，深色 %d，沉浸 %d",
             s_state.book_count, s_state.font_size, s_state.backlight,
             s_state.dark, s_state.immersive);
    return ESP_OK;
}

void reader_app_deinit(void)
{
    s_ready = false;
    pagemap_free();
    reader_transfer_stop();
    s_scroll_dir = 0;
    if (s_tick_timer || s_scroll_timer) {
        if (bsp_lvgl_lock(1000)) {
            if (s_scroll_timer) lv_timer_delete(s_scroll_timer);
            if (s_tick_timer) lv_timer_delete(s_tick_timer);
            reader_ui_deinit();  // stops the screen after its timers are gone
            bsp_lvgl_unlock();
        }
        s_scroll_timer = NULL;
        s_tick_timer = NULL;
    } else {
        reader_ui_deinit();
    }
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}
