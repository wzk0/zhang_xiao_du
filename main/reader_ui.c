// reader_ui.c — a flat, quiet interface. See reader_ui.h.
//
// Visual language: one near-white surface, one ink colour, one accent. No
// borders, no card fills, no shadows — hierarchy comes from type size, spacing
// and a single soft pill behind the current selection. This is a deliberate
// departure from the baseline demo's pixel-art shell and from the earlier
// "paper card" look, which read as busy on a 240x320 panel.
#include "reader_ui.h"

#include "reader_fonts.h"
#include "reader_layout.h"
#include "reader_storage.h"
#include "reader_transfer.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// Two palettes share one widget tree: every C_* name resolves through g_pal,
// which new_screen() refreshes from s_dark before any widget is built. Keeping
// the names means the individual screens never branch on the theme.
typedef struct {
    lv_color_t paper;    // page background
    lv_color_t surface;  // selection pill
    lv_color_t ink;      // primary text (book titles)
    lv_color_t sub;      // secondary text
    lv_color_t accent;   // selected text, progress fill, hairline accents
    lv_color_t action;   // cool blue emphasis for action rows (not a book title)
    lv_color_t line;     // empty track / dividers
} reader_palette_t;

static reader_palette_t g_pal;
static bool s_dark;

// Transient reader-footer message owned by the application. 32 B holds the
// longest hint ("使用指南不可删除", eight Hanzi) without truncation.
static char s_hint[32];

#define C_PAPER   (g_pal.paper)
#define C_SURFACE (g_pal.surface)
#define C_INK     (g_pal.ink)
#define C_SUB     (g_pal.sub)
#define C_ACCENT  (g_pal.accent)
#define C_ACTION  (g_pal.action)
#define C_LINE    (g_pal.line)

static void apply_palette(void)
{
    if (s_dark) {
        g_pal.paper   = lv_color_hex(0x121110);
        g_pal.surface = lv_color_hex(0x26221B);
        g_pal.ink     = lv_color_hex(0xEDE7DD);
        g_pal.sub     = lv_color_hex(0x8B8377);
        g_pal.accent  = lv_color_hex(0xC8795F);
        g_pal.action  = lv_color_hex(0x8FB3D9);  // cool blue-grey, not ink
        g_pal.line    = lv_color_hex(0x2E2A24);
    } else {
        g_pal.paper   = lv_color_hex(0xFBF9F5);
        g_pal.surface = lv_color_hex(0xEFEAE1);
        g_pal.ink     = lv_color_hex(0x1D1B18);
        g_pal.sub     = lv_color_hex(0x8B8378);
        g_pal.accent  = lv_color_hex(0xA6482E);
        g_pal.action  = lv_color_hex(0x3F6C93);  // cool blue-grey, not ink
        g_pal.line    = lv_color_hex(0xE5DED2);
    }
}

void reader_ui_set_dark(bool dark)
{
    s_dark = dark;
}

void reader_ui_set_hint(const char *text)
{
    snprintf(s_hint, sizeof(s_hint), "%s", text ? text : "");
}

#define UI_W 240
#define UI_H 320
#define UI_X 18            // single horizontal margin used by every screen
#define UI_PILL_X 10       // selection pill extends past the text margin
#define UI_LIST_ROWS 5
#define UI_ROW_H 40

// The interface is chrome, not content: three fixed steps, all smaller than the
// default body size so the reading page keeps the visual weight. Body text on
// the reader page is owned by the font picker instead (see reader_layout.h).
#define F_TITLE 17
#define F_BODY  15
#define F_SMALL 12
#define F_NUM   20

static lv_obj_t *s_screen;
static reader_screen_t s_current = READER_SCREEN_LIBRARY;
static lv_obj_t *s_battery;
static lv_obj_t *s_load_status;
static lv_obj_t *s_load_bar;

// Transfer-page widgets, refreshed in place by the tick timer. Only the value
// labels are retained: captions and the "total/used" detail were removed to
// keep this page quiet and vertically centred.
static lv_obj_t *s_t_free;
static lv_obj_t *s_t_bar;
static lv_obj_t *s_t_status;
static lv_obj_t *s_t_detail;
static lv_obj_t *s_t_conn;

void reader_ui_init(void)
{
    s_screen = NULL;
    s_battery = NULL;
    s_load_status = s_load_bar = NULL;
    s_current = READER_SCREEN_LIBRARY;
    s_dark = false;
    apply_palette();
}

void reader_ui_deinit(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
}

// ---------------------------------------------------------------------------
// Shared building blocks
// ---------------------------------------------------------------------------
static lv_obj_t *new_screen(reader_screen_t screen)
{
    // Refresh the palette first: every widget built below reads C_* from it.
    apply_palette();
    if (s_screen) lv_obj_delete(s_screen);
    s_battery = NULL;
    s_t_free = s_t_bar = s_t_status = s_t_detail = s_t_conn = NULL;
    s_current = screen;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, UI_W, UI_H);
    lv_obj_set_style_bg_color(scr, C_PAPER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    s_screen = scr;
    return scr;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, int font_size,
                       lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, reader_font(font_size), 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    return l;
}

// A hairline, used only where two unrelated blocks would otherwise run together.
static lv_obj_t *hairline(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, 1);
    lv_obj_set_style_bg_color(r, C_LINE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    return r;
}

// The only decoration in the app: a soft filled pill behind the current row.
static lv_obj_t *row(lv_obj_t *parent, int x, int y, int w, int h, bool sel)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (sel) {
        lv_obj_set_style_bg_color(r, C_SURFACE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 9, 0);
    }
    return r;
}

// A centred label on the shared column. Most quiet pages use it for one-line
// captions; keeping the alignment here avoids repeating three LVGL calls at
// every call site.
static lv_obj_t *centered_label(lv_obj_t *parent, int x, int y, int w,
                                int font_size, lv_color_t color,
                                const char *text)
{
    lv_obj_t *l = label(parent, x, y, w, font_size, color);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

// A two-part progress bar: a hairline track plus a filled share. The reader,
// storage and transfer pages all draw the same widget at different heights;
// one helper keeps their radii and width arithmetic identical.
static lv_obj_t *bar(lv_obj_t *scr, int x, int y, int w, int h, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    lv_obj_t *track = lv_obj_create(scr);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, x, y);
    lv_obj_set_size(track, w, h);
    lv_obj_set_style_bg_color(track, C_LINE, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, h / 2, 0);

    lv_obj_t *fill = lv_obj_create(scr);
    lv_obj_remove_style_all(fill);
    lv_obj_set_pos(fill, x, y);
    lv_obj_set_size(fill, (int32_t)((unsigned)w * (unsigned)pct / 100u), h);
    lv_obj_set_style_bg_color(fill, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, h / 2, 0);
    return fill;
}

void reader_ui_update_loading(const char *title, const char *phase, unsigned percent)
{
    if (!s_screen) return;
    const int panel_w = UI_W - 2 * UI_X;
    const int content_w = panel_w - 24;
    if (!s_load_status) {
        lv_obj_t *shade = lv_obj_create(s_screen);
        lv_obj_remove_style_all(shade);
        lv_obj_set_size(shade, UI_W, UI_H);
        lv_obj_set_style_bg_color(shade, C_INK, 0);
        lv_obj_set_style_bg_opa(shade, LV_OPA_30, 0);
        lv_obj_t *panel = row(shade, UI_X, 112, panel_w, 96, true);
        lv_obj_set_style_bg_color(panel, C_PAPER, 0);
        (void)centered_label(panel, 12, 12, content_w, F_BODY, C_INK, title);
        s_load_status = centered_label(panel, 12, 43, content_w, F_SMALL, C_SUB, "");
        s_load_bar = bar(panel, 12, 69, content_w, 4, 0);
    }

    if (percent > 100) percent = 100;
    char text[48];
    snprintf(text, sizeof(text), "%s · %u%%", phase, percent);
    lv_label_set_text(s_load_status, text);
    lv_obj_set_width(s_load_bar, (int32_t)((unsigned)content_w * percent / 100u));
}

static void header(lv_obj_t *scr, const char *title, const char *subtitle,
                   const char *battery)
{
    lv_obj_t *t = label(scr, UI_X, 14, 170, F_TITLE, C_INK);
    lv_label_set_text(t, title);
    if (subtitle && subtitle[0]) {
        lv_obj_t *s = label(scr, UI_X, 38, 190, F_SMALL, C_SUB);
        lv_label_set_text(s, subtitle);
    }

    // The application formats the battery string, including its "no gauge"
    // fallback; the UI only places it.
    s_battery = label(scr, 150, 18, UI_W - UI_X - 150, F_SMALL, C_SUB);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_battery, battery ? battery : "");
}

// Key hints are distinct chips rather than one run of text: wrapping each
// action in its own component makes the operation obvious at a glance.
#define KEY_PAD 9
#define KEY_GAP 8
#define KEY_H   22
#define KEY_Y  288

static int key_text_width(const char *text)
{
    lv_point_t size;
    // COORD_MAX keeps the measurement on one line; the chips size themselves
    // from the result, so no hint is ever clipped or given a sloppy fixed width.
    lv_text_get_size(&size, text, reader_font(F_SMALL), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)size.x;
}

static void key_pill(lv_obj_t *scr, const char *text, int x, int w, int pad)
{
    lv_obj_t *chip = lv_obj_create(scr);
    lv_obj_remove_style_all(chip);
    lv_obj_set_pos(chip, x, KEY_Y);
    lv_obj_set_size(chip, w, KEY_H);
    lv_obj_set_style_bg_color(chip, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 8, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(chip);
    lv_obj_set_pos(l, pad, 3);
    lv_obj_set_width(l, w - 2 * pad);
    lv_obj_set_style_text_font(l, reader_font(F_SMALL), 0);
    lv_obj_set_style_text_color(l, C_ACCENT, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, text);
}

// Lay out up to three chips from their measured widths. The shelf and every
// other screen use two chips, sized from the real font metrics so no hint is
// clipped on the 240 px panel.
static void footer_n(lv_obj_t *scr, const char *const *texts, int n,
                     int pad, int gap)
{
    int widths[3] = { 0, 0, 0 };
    int total = 0;
    for (int i = 0; i < n; ++i) {
        if (!texts[i] || !texts[i][0]) continue;
        widths[i] = key_text_width(texts[i]) + 2 * pad;
        total += widths[i] + (i ? gap : 0);
    }
    int x = (UI_W - total) / 2;
    if (x < 4) x = 4;
    for (int i = 0; i < n; ++i) {
        if (!widths[i]) continue;
        key_pill(scr, texts[i], x, widths[i], pad);
        x += widths[i] + gap;
    }
}

static void footer(lv_obj_t *scr, const char *left, const char *right)
{
    const char *texts[2] = { left, right };
    footer_n(scr, texts, 2, KEY_PAD, KEY_GAP);
}

static void format_size(size_t bytes, char *out, size_t out_size)
{
    if (bytes >= 1024u * 1024u) {
        size_t mb = bytes / (1024u * 1024u);
        size_t frac = (bytes % (1024u * 1024u)) * 100u / (1024u * 1024u);
        snprintf(out, out_size, "%u.%02u MB", (unsigned)mb, (unsigned)frac);
    } else if (bytes >= 1024u) {
        size_t kb = bytes / 1024u;
        size_t frac = (bytes % 1024u) * 10u / 1024u;
        snprintf(out, out_size, "%u.%u KB", (unsigned)kb, (unsigned)frac);
    } else {
        snprintf(out, out_size, "%u B", (unsigned)bytes);
    }
}

static void show(lv_obj_t *scr)
{
    lv_screen_load(scr);
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------
// Draw one shelf row. `list_index` follows the application's cursor:
// 0..count-1 map directly to books[list_index], and `count` is the single
// pinned settings action at the bottom of the shelf.
static void library_row(lv_obj_t *scr, int y, int list_index, int count,
                        const reader_book_info_t *books, bool is_sel)
{
    const bool is_action = (list_index == count);
    lv_obj_t *r = row(scr, UI_PILL_X, y, UI_W - 2 * UI_PILL_X, UI_ROW_H,
                      is_sel);

    // The action row uses its own cool blue emphasis instead of ink, so it
    // cannot be mistaken for a book title. Selection still turns the label
    // to the accent colour, on top of the pill behind it.
    lv_obj_t *name = label(r, 8, 3, 204, F_BODY,
                           is_sel ? C_ACCENT
                                  : (is_action ? C_ACTION : C_INK));
    lv_obj_t *meta = label(r, 8, 21, 204, F_SMALL, C_SUB);

    if (is_action) {
        lv_label_set_text(name, "阅读设置");
        lv_label_set_text(meta, "字号 · 亮度 · 深色 · 沉浸 · 存储");
    } else {
        const reader_book_info_t *b = &books[list_index];
        lv_label_set_text(name, b->title);
        char m[40], sz[24];
        format_size(b->size, sz, sizeof(sz));
        if (b->builtin) {
            snprintf(m, sizeof(m), "内置示例 · %s", sz);
        } else {
            snprintf(m, sizeof(m), "已上传 · %s", sz);
        }
        lv_label_set_text(meta, m);
    }
}

void reader_ui_render_library(const reader_book_info_t *books, int count,
                              int selected, const char *battery)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_LIBRARY);
    char sub[48];
    // Only the count lives in the subtitle: destructive gestures belong with
    // the other key hints at the bottom, not hidden in the header.
    snprintf(sub, sizeof(sub), "%d 本书", count);
    header(scr, "掌小读", sub, battery);

    // One action row is PINNED at the bottom (阅读设置). The four rows above it
    // scroll through the books, so with many books the settings row still
    // never moves off screen. 接收新书 now lives inside that settings page.
    const int mid_rows = UI_LIST_ROWS - 1;  // 4
    const int step = UI_ROW_H + 4;
    library_row(scr, 64 + (UI_LIST_ROWS - 1) * step,
                count, count, books, selected == count);

    int start = 0;
    if (count > mid_rows) {
        start = selected - mid_rows / 2;  // centre the selected book
        if (start < 0) start = 0;
        if (start > count - mid_rows) start = count - mid_rows;
    }
    for (int i = 0; i < mid_rows && start + i < count; ++i) {
        int list_index = start + i;
        library_row(scr, 64 + i * step, list_index, count, books,
                    selected == list_index);
    }

    // Long-press deletion is folded into the first operation chip, so the shelf
    // is back to exactly two hints: select/long-press and open.
    if (s_hint[0]) footer(scr, s_hint, NULL);
    else footer(scr, "上/下 选择 长按删除", "确定 打开");
    show(scr);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
void reader_ui_render_reader(const char *title, const char *chapter,
                             const char *page_text, int font_size,
                             int percent, const char *battery, int auto_mode,
                             int immersive)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_READER);
    const reader_font_metrics_t *m = reader_layout_metrics(font_size);
    const lv_font_t *font = reader_font(m->size);

    // Immersive mode is about MORE TEXT, not more whitespace: it drops the
    // chrome and spends the freed height on extra lines. With a 16 px top inset
    // and a box reaching y=288, the last line now ends at y=272..280 (12..20 px)
    // instead of the old 262 — the bottom blank nearly matches the top inset,
    // and even the default 16 px font gains a line. Both the paginator and this
    // renderer read reader_layout_immersive_lines(), so what is laid out is
    // exactly what is drawn — a mismatch here would clip text and make a
    // bookmark look like it jumped to the wrong sentence.
    if (immersive) {
        const int lines = reader_layout_immersive_lines(font_size);
        const int body_h = lines * (int)m->pitch;
        lv_obj_t *body = lv_label_create(scr);
        lv_obj_set_pos(body, UI_X, READER_LAYOUT_IMMERSIVE_TOP);
        lv_obj_set_width(body, READER_LAYOUT_BODY_W);
        lv_obj_set_height(body, body_h);
        lv_obj_set_style_text_font(body, font, 0);
        lv_obj_set_style_text_color(body, C_INK, 0);
        lv_obj_set_style_text_line_space(
            body, m->pitch - (int32_t)font->line_height, 0);
        lv_label_set_long_mode(body, LV_LABEL_LONG_CLIP);
        lv_label_set_text(body, page_text ? page_text : "");
        // The only chrome allowed here: the transient "已添加书签" toast, in
        // the reserved bottom band. Regular key hints stay hidden.
        if (s_hint[0]) footer(scr, s_hint, NULL);
        show(scr);
        return;
    }

    // The chapter is the header subtitle, so it sits directly under the book
    // name in the same slot the other screens use for their captions (y=38).
    // A separate lower caption at y=58 made the heading look sunk.
    header(scr, title ? title : "阅读", chapter ? chapter : NULL, battery);

    // The chapter now lives in the header subtitle (y=38), so the body can
    // start at y=68 instead of 84. That puts ~14 px above the text and 8..20 px
    // below it (before the 已读 label at 256), i.e. a near-symmetric block, and
    // buys one extra line at most sizes. Keep this in step with BODY_H=188.
    lv_obj_t *body = lv_label_create(scr);
    lv_obj_set_pos(body, UI_X, 68);
    lv_obj_set_width(body, READER_LAYOUT_BODY_W);
    lv_obj_set_height(body, (int32_t)m->lines * m->pitch);
    lv_obj_set_style_text_font(body, font, 0);
    lv_obj_set_style_text_color(body, C_INK, 0);
    // Derive spacing from the font LVGL actually uses, not from its pixel size.
    lv_obj_set_style_text_line_space(body, m->pitch - (int32_t)font->line_height, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_CLIP);
    lv_label_set_text(body, page_text ? page_text : "");

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char progress[40];
    snprintf(progress, sizeof(progress), "已读 %d%%%s", percent,
             auto_mode ? " · 自动翻页" : "");
    lv_obj_t *pl = label(scr, UI_X, 256, READER_LAYOUT_BODY_W, F_SMALL, C_SUB);
    lv_label_set_text(pl, progress);

    // 2 px track: a reading-progress hairline, not a chart.
    (void)bar(scr, UI_X, 276, READER_LAYOUT_BODY_W, 2, percent);

    if (s_hint[0]) {
        footer(scr, s_hint, NULL);
    } else if (percent >= 100) {
        // One more DOWN opens the "完" confirmation, not the shelf directly.
        footer(scr, "下键 结束阅读", "确定 目录");
    } else {
        footer(scr, "长按上 书签", "长按下 设置");
    }
    show(scr);
}

// ---------------------------------------------------------------------------
// End-of-book confirmation
// ---------------------------------------------------------------------------
void reader_ui_render_finished(const char *book_title)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_FINISHED);
    // A single character, larger than any chrome text, centred on the panel.
    // No header, no battery, no progress: this page is a deliberate pause.
    // The book title sits quietly underneath so the reader knows what ended.
    // No header here, so centre the whole block in 0..288 (the key chips start
    // at 288): 完 at 118 and the title at 156 put the group's centre at ~145.
    (void)centered_label(scr, UI_X, 118, UI_W - 2 * UI_X, F_NUM, C_INK, "完");
    if (book_title && book_title[0]) {
        (void)centered_label(scr, UI_X, 156, UI_W - 2 * UI_X, F_SMALL, C_SUB,
                             book_title);
    }
    footer(scr, "按 下/确定 返回书柜", NULL);
    show(scr);
}

// ---------------------------------------------------------------------------
// Title card — page 0 of every book, reachable by pressing UP on page 1
// ---------------------------------------------------------------------------
void reader_ui_render_cover(const char *book_title, size_t char_count,
                            size_t flash_bytes)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_COVER);
    // Same quiet grammar as the 完 page: one large centred word, a small
    // subtitle and a single key chip. Uploaded filenames can be long, so the
    // title wraps within a fixed band instead of being clipped to one line.
    lv_obj_t *w = label(scr, UI_X, 106, UI_W - 2 * UI_X, F_NUM, C_INK);
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(w, 100);
    lv_obj_set_style_text_line_space(w, 4, 0);
    lv_label_set_text(w, book_title && book_title[0] ? book_title : "掌小读");

    // Subtitle is book data, not app branding: word count plus a readable
    // size. char_count is 0 only during the first frame of a first open,
    // before the index scan has finished; show the size alone in that frame
    // rather than a misleading "0字".
    char sz[24], sub_text[48];
    format_size(flash_bytes, sz, sizeof(sz));
    if (char_count) {
        snprintf(sub_text, sizeof(sub_text), "%u字 · %s",
                 (unsigned)char_count, sz);
    } else {
        snprintf(sub_text, sizeof(sub_text), "%s", sz);
    }
    (void)centered_label(scr, UI_X, 218, UI_W - 2 * UI_X, F_SMALL, C_SUB,
                         sub_text);
    footer(scr, "按 确定/下 开始阅读", NULL);
    show(scr);
}

// ---------------------------------------------------------------------------
// Delete confirmation — one deliberate pause before any destructive action
// ---------------------------------------------------------------------------
void reader_ui_render_confirm(const char *question, const char *item,
                              const char *battery)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_CONFIRM);
    header(scr, question ? question : "删除确认", NULL, battery);

    // The name (a book title or the bookmark's text excerpt) is what the user
    // must recognise before agreeing; it wraps rather than being cut off.
    // Shifted down 12 px so the name + warning group (120..222) is centred in
    // the usable band between the header (54) and the key chips (288).
    lv_obj_t *name = label(scr, UI_X, 120, UI_W - 2 * UI_X, F_BODY, C_INK);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(name, 64);
    lv_obj_set_style_text_line_space(name, 2, 0);
    lv_label_set_text(name, item && item[0] ? item : "");

    (void)centered_label(scr, UI_X, 206, UI_W - 2 * UI_X, F_SMALL, C_SUB,
                         "删除后无法恢复");
    footer(scr, "确定 删除", "上/下 取消");
    show(scr);
}

// ---------------------------------------------------------------------------
// Table of contents
// ---------------------------------------------------------------------------
void reader_ui_render_toc(const reader_book_index_t *index,
                          int mark_count, const char *const *mark_titles,
                          int mode, int focus, int selected,
                          const char *battery)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_TOC);
    const int chapters = index ? index->count : 0;
    // No "book name - N chapters" caption above the tabs: the counts now live
    // inside the tabs themselves.
    header(scr, "目录", NULL, battery);

    // Two equal-width tabs centred as a pair. 2*96 + 12 = 204 px, which leaves
    // exactly UI_X (18 px) on each side of the 240 px panel.
    const int tab_w = 96, gap = 12;
    const int x0 = (UI_W - (2 * tab_w + gap)) / 2;
    char tab_names[2][24];
    snprintf(tab_names[0], sizeof(tab_names[0]), "目录(%d章)", chapters);
    snprintf(tab_names[1], sizeof(tab_names[1]), "书签(%d个)", mark_count);
    for (int t = 0; t < 2; ++t) {
        bool active = t == mode;
        int x = x0 + t * (tab_w + gap);
        (void)centered_label(scr, x, 58, tab_w, F_BODY,
                             active ? C_INK : C_SUB, tab_names[t]);
        if (active) {
            // A wider underline marks the "picking a tab" focus; a narrow one
            // simply marks the active tab.
            lv_obj_t *u = lv_obj_create(scr);
            lv_obj_remove_style_all(u);
            int uw = focus == 0 ? tab_w : 24;
            lv_obj_set_pos(u, x + (tab_w - uw) / 2, 81);
            lv_obj_set_size(u, uw, 2);
            lv_obj_set_style_bg_color(u, C_ACCENT, 0);
            lv_obj_set_style_bg_opa(u, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(u, 1, 0);
        }
    }

    const int count = mode == 0 ? chapters : mark_count;
    if (count == 0) {
        lv_obj_t *t = label(scr, UI_X, 100, UI_W - 2 * UI_X, F_SMALL, C_SUB);
        lv_obj_set_style_text_line_space(t, -1, 0);
        lv_label_set_text(t, mode == 0 ? "这本书没有可识别的章节标题"
                                       : "还没有书签\n阅读时长按上键添加");
    } else {
        // Four rows fit between the tab underline (y=81) and the footer hint.
        const int rows = 4;
        int start = 0;
        if (count > rows) {
            start = selected - rows / 2;
            if (start < 0) start = 0;
            if (start > count - rows) start = count - rows;
        }
        for (int i = 0; i < rows && start + i < count; ++i) {
            int idx = start + i;
            int y = 96 + i * (UI_ROW_H + 4);
            bool sel = focus == 1 && idx == selected;
            lv_obj_t *r = row(scr, UI_PILL_X, y, UI_W - 2 * UI_PILL_X, UI_ROW_H,
                              sel);
            lv_obj_t *num = label(r, 8, 12, 28, F_SMALL,
                                  sel ? C_ACCENT : C_SUB);
            lv_label_set_text_fmt(num, "%02d", idx + 1);
            lv_obj_t *t = label(r, 38, 9, 174, F_BODY,
                                sel ? C_ACCENT : C_INK);
            if (mode == 0) {
                lv_label_set_text(t, index->items[idx].title);
            } else {
                // The label is the sentence AT the bookmark, generated by the
                // application from the book text. A chapter heading would be
                // identical for every bookmark in the same chapter.
                const char *name = "位置";
                if (mark_titles && mark_titles[idx] && mark_titles[idx][0]) {
                    name = mark_titles[idx];
                }
                lv_label_set_text(t, name);
            }
        }
    }
    if (s_hint[0]) {
        footer(scr, s_hint, NULL);
    } else if (focus == 0) {
        footer(scr, "上/下 切换标签", "确定 进入");
    } else if (mode == 1) {
        footer(scr, "长按上/下 删除", "长按确定 返回");
    } else {
        footer(scr, "确定 跳转", "长按 返回");
    }
    show(scr);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
void reader_ui_render_settings(int selected, int font_size, int backlight,
                               int idle_seconds, int dark, int immersive,
                               const char *battery)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_SETTINGS);
    header(scr, "设置", NULL, battery);

    // 接收新书 is pinned at the TOP of settings: the shelf no longer carries
    // its own action row. The remaining six rows are the reader options.
    const char *names[7] = { "接收新书", "正文字号", "屏幕亮度", "自动息屏",
                             "深色模式", "沉浸模式", "存储空间" };
    char values[7][24];
    // The ">" marks rows 0, 1 and 6 as sub-page entries rather than toggles.
    snprintf(values[0], sizeof(values[0]), "%s", "开启热点 >");
    snprintf(values[1], sizeof(values[1]), "%d px >", font_size);
    snprintf(values[2], sizeof(values[2]), "%d%%", backlight);
    snprintf(values[3], sizeof(values[3]), "%d 秒", idle_seconds);
    snprintf(values[4], sizeof(values[4]), "%s", dark ? "开" : "关");
    snprintf(values[5], sizeof(values[5]), "%s", immersive ? "开" : "关");
    snprintf(values[6], sizeof(values[6]), "%s", "查看剩余 >");

    // Seven compact 27 px rows on a 31 px grid: 58..271, clear of the footer
    // at 288. Adding the transfer row must not push a row under the chips.
    for (int i = 0; i < 7; ++i) {
        int y = 58 + i * 31;
        bool sel = i == selected;
        lv_obj_t *r = row(scr, UI_PILL_X, y, UI_W - 2 * UI_PILL_X, 27, sel);
        // 接收新书 (row 0) is an action, not a toggle: give it the same cool
        // blue emphasis as the shelf's action row so it stands out. A
        // selected row always uses the accent colour so focus stays uniform.
        lv_color_t name_color = sel ? C_ACCENT
                                    : (i == 0 ? C_ACTION : C_INK);
        lv_obj_t *n = label(r, 8, 4, 120, F_BODY, name_color);
        lv_label_set_text(n, names[i]);
        lv_obj_t *v = label(r, 8, 4, 204, F_BODY, C_SUB);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(v, values[i]);
    }
    footer(scr, "确定 调整", "长按 返回");
    show(scr);
}

// ---------------------------------------------------------------------------
// Storage — remaining space, percentages and a generated hint
// ---------------------------------------------------------------------------
void reader_ui_render_storage(size_t total, size_t used, size_t free_bytes,
                              const char *hint, const char *battery)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_STORAGE);
    header(scr, "存储空间", NULL, battery);

    if (!total) {
        (void)centered_label(scr, UI_X, 140, UI_W - 2 * UI_X,
                             F_BODY, C_INK, "空间读取失败");
        footer(scr, "长按确定", "返回");
        show(scr);
        return;
    }

    char free_text[32], total_text[24], used_text[24];
    format_size(free_bytes, free_text, sizeof(free_text));
    format_size(total, total_text, sizeof(total_text));
    format_size(used, used_text, sizeof(used_text));

    lv_obj_t *cap = label(scr, UI_X, 66, UI_W - 2 * UI_X, F_SMALL, C_SUB);
    lv_label_set_text(cap, "剩余空间");
    lv_obj_t *big = label(scr, UI_X, 86, UI_W - 2 * UI_X, F_NUM, C_ACCENT);
    lv_label_set_text(big, free_text);

    // A saturated percentage keeps a failed SPIFFS query (total == 0) from
    // dividing by zero or printing a nonsense value.
    int used_pct = reader_storage_percent(used, total);
    // Derive the free share from the used share so the two numbers always add
    // up to 100 instead of losing a point to independent integer truncation.
    int free_pct = total ? 100 - used_pct : 0;
    char pct[48];
    snprintf(pct, sizeof(pct), "剩余 %d%% · 已用 %d%%", free_pct, used_pct);
    lv_obj_t *p = label(scr, UI_X, 116, UI_W - 2 * UI_X, F_BODY, C_INK);
    lv_label_set_text(p, pct);

    // The filled part is the USED share; the track is the remaining share. Both
    // are derived from the same total, so the bar and the numbers agree.
    const int bar_w = 204, bar_y = 146, bar_h = 6;
    (void)bar(scr, UI_X, bar_y, bar_w, bar_h, used_pct);

    char detail[96];
    snprintf(detail, sizeof(detail), "总计 %s · 已用 %s", total_text, used_text);
    lv_obj_t *d = label(scr, UI_X, 164, UI_W - 2 * UI_X, F_SMALL, C_SUB);
    lv_label_set_text(d, detail);

    lv_obj_t *h = label(scr, UI_X, 194, UI_W - 2 * UI_X, F_BODY, C_INK);
    // label() defaults to CLIP; the generated hint is one or two lines long.
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(h, 68);
    lv_obj_set_style_text_line_space(h, 3, 0);
    lv_label_set_text(h, hint && hint[0] ? hint : "暂时无法读取存储信息");

    footer(scr, "长按确定 返回", NULL);
    show(scr);
}

// ---------------------------------------------------------------------------
// Font-size picker — a live preview of the reading page
// ---------------------------------------------------------------------------
// A short classic-verse sample: every glyph is in the committed inventory and
// it is long enough to show several line breaks at the largest size.
static const char *const FONT_PREVIEW =
    "床前明月光，疑是地上霜。举头望明月，低头思故乡。"
    "春眠不觉晓，处处闻啼鸟。";

void reader_ui_render_font_picker(int font_size, const char *battery)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_FONT_PICKER);
    const reader_font_metrics_t *m = reader_layout_metrics(font_size);

    // No caption: the numeric size and the live sample say everything. Keeping
    // the header to a single line is the whole point of this quiet UI.
    header(scr, "正文字号", NULL, battery);

    // The preview deliberately reuses the reader page geometry: the same 204 px
    // column, the same font and the same baseline pitch. It simply starts a
    // little higher because this screen has no header subtitle (the reading
    // page shows book name + chapter there). Anything else would make the
    // picker lie about the page it is previewing.
    lv_obj_t *body = label(scr, UI_X, 64, READER_LAYOUT_BODY_W, m->size, C_INK);
    // label() defaults to CLIP, which is right for pre-paginated pages (they
    // already contain newlines) but leaves this continuous sample on one line.
    // WRAP makes it reflow on the same 204 px column as the real reading page.
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(body, 128);
    lv_obj_set_style_text_line_space(
        body, m->pitch - (int32_t)reader_font(m->size)->line_height, 0);
    lv_label_set_text(body, FONT_PREVIEW);
    hairline(scr, UI_X, 198, UI_W - 2 * UI_X);

    // Numeric readout: the size is what the user is choosing, so it is the
    // largest glyph on the screen.
    char value[24];
    snprintf(value, sizeof(value), "%d", (int)m->size);
    (void)centered_label(scr, UI_X, 206, UI_W - 2 * UI_X, F_NUM, C_ACCENT,
                         value);

    // Nine dots: one per selectable size, filled up to the current one, so the
    // position inside the range is readable at a glance.
    const int steps = READER_FONT_MAX_SIZE - READER_FONT_MIN_SIZE + 1;
    const int dot = 6, gap = 8;
    const int total_w = steps * dot + (steps - 1) * gap;
    const int x0 = UI_X + (UI_W - 2 * UI_X - total_w) / 2;
    for (int i = 0; i < steps; ++i) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_pos(d, x0 + i * (dot + gap), 242);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_style_radius(d, dot / 2, 0);
        bool on = i <= font_size - READER_FONT_MIN_SIZE;
        lv_obj_set_style_bg_color(d, on ? C_ACCENT : C_LINE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }

    footer(scr, "上/下 调字号", "确定 保存返回");
    show(scr);
}

// ---------------------------------------------------------------------------
// Transfer — the remaining-storage screen required by the user
// ---------------------------------------------------------------------------
static void fill_transfer_labels(const reader_transfer_status_t *status,
                                 size_t total, size_t used, size_t free_bytes)
{
    char free_text[32], conn[64], state[80];
    format_size(free_bytes, free_text, sizeof(free_text));

    const char *space = total ? free_text : "空间读取失败";
    if (strcmp(lv_label_get_text(s_t_free), space) != 0) {
        lv_label_set_text(s_t_free, space);
    }
    if (s_t_bar) {
        unsigned pct = total ? (unsigned)((uint64_t)used * 100u / total) : 0;
        if (pct > 100) pct = 100;
        lv_obj_set_size(s_t_bar, (int32_t)(204u * pct / 100u), 4);
    }
    if (s_t_conn) {
        if (!status->active) {
            snprintf(conn, sizeof(conn), "%s", "热点启动失败");
        } else {
            snprintf(conn, sizeof(conn), "%s · %u 台",
                     status->client_ready ? "手机已就绪" : "等待连接",
                     (unsigned)status->client_count);
        }
        if (strcmp(lv_label_get_text(s_t_conn), conn) != 0) {
            lv_label_set_text(s_t_conn, conn);
        }
    }
    const char *result = "尚未收到书籍";
    switch (status->upload) {
    case READER_UPLOAD_RECEIVING:
        snprintf(state, sizeof(state), "正在接收 %u%%", (unsigned)status->upload_percent);
        result = state;
        break;
    case READER_UPLOAD_PAUSED:
        snprintf(state, sizeof(state), "等待续传 %u%%", (unsigned)status->upload_percent);
        result = state;
        break;
    case READER_UPLOAD_VERIFYING:
        result = "正在校验书籍";
        break;
    case READER_UPLOAD_SAVED:
        result = "上传成功";
        if (status->unsupported) {
            snprintf(state, sizeof(state), "%s · %u 个字缺字形", result,
                     (unsigned)status->unsupported);
            result = state;
        }
        break;
    case READER_UPLOAD_FAILED:
        result = "上传失败";
        break;
    case READER_UPLOAD_IDLE:
        break;
    }
    if (strcmp(lv_label_get_text(s_t_status), result) != 0) {
        lv_obj_set_style_text_color(s_t_status,
            status->upload == READER_UPLOAD_SAVED ? C_ACCENT : C_SUB, 0);
        lv_label_set_text(s_t_status, result);
    }
    if (strcmp(lv_label_get_text(s_t_detail), status->upload_detail) != 0) {
        lv_label_set_text(s_t_detail, status->upload_detail);
    }
}

void reader_ui_render_transfer(const reader_transfer_status_t *status,
                               size_t total, size_t used, size_t free_bytes,
                               const char *battery)
{
    lv_obj_t *scr = new_screen(READER_SCREEN_TRANSFER);
    header(scr, "接收新书", NULL, battery);

    const int w = UI_W - 2 * UI_X;
    (void)centered_label(scr, UI_X, 80, w, F_BODY, C_INK, READER_AP_SSID);
    (void)centered_label(scr, UI_X, 104, w, F_BODY, C_ACCENT, READER_AP_URL);
    (void)centered_label(scr, UI_X, 139, w, F_SMALL, C_SUB, "剩余空间");
    s_t_free = centered_label(scr, UI_X, 157, w, F_NUM, C_INK, "");
    s_t_bar = bar(scr, UI_X, 191, w, 4, 0);

    s_t_conn = centered_label(scr, UI_X, 208, w, F_SMALL, C_SUB, "");
    s_t_status = centered_label(scr, UI_X, 232, w, F_SMALL, C_SUB, "");
    s_t_detail = centered_label(scr, UI_X, 250, w, F_SMALL, C_INK, "");
    lv_label_set_long_mode(s_t_detail, LV_LABEL_LONG_WRAP);

    fill_transfer_labels(status, total, used, free_bytes);
    footer(scr, "长按确定关闭热点", NULL);
    show(scr);
}

void reader_ui_refresh_transfer(const reader_transfer_status_t *status,
                                size_t total, size_t used, size_t free_bytes)
{
    if (s_current != READER_SCREEN_TRANSFER || !s_t_free) return;
    fill_transfer_labels(status, total, used, free_bytes);
}

void reader_ui_set_battery(const char *text)
{
    if (!s_battery) return;
    lv_label_set_text(s_battery, text ? text : "");
}
