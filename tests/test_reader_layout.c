// Host tests for the per-size page geometry table in reader_layout.c.
//
// The table is the agreement between three consumers: the paginator (columns
// and lines), the reader screen (line pitch) and the font picker preview. A bad
// row would either clip the last line into the progress bar or push text past
// the right margin, and neither is visible from a host test that only builds the
// UI. These checks pin the arithmetic instead.
// Build: cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_reader_layout.c
//        main/reader_layout.c main/reader_text.c -o /tmp/test_reader_layout
#include "reader_layout.h"
#include "reader_text.h"

#include <stdio.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

#define ROW_COUNT READER_FONT_SIZE_COUNT

static void test_table_length(void)
{
    // Seven selectable sizes: 17 px and 19 px would only duplicate the upscaled
    // glyphs 18 px and 20 px already render more cleanly.
    CHECK(ROW_COUNT == 7);
    CHECK(reader_layout_size_valid(READER_FONT_DEFAULT_SIZE) == 1);
    CHECK(g_reader_font_sizes[0] == READER_FONT_MIN_SIZE);
    CHECK(g_reader_font_sizes[ROW_COUNT - 1] == READER_FONT_MAX_SIZE);
}

static void test_rows_are_consistent(void)
{
    for (int i = 0; i < ROW_COUNT; ++i) {
        const reader_font_metrics_t *m = &g_reader_font_metrics[i];
        // One row per selectable size, ascending, and the row's size must be
        // the table entry the picker steps through.
        CHECK(m->size == g_reader_font_sizes[i]);
        if (i) CHECK(m->size > g_reader_font_sizes[i - 1]);
        // The generator emits line_height = size + 4 and the UI feeds the
        // paginator the same value as the baseline pitch, so text_line_space
        // stays 0 at every size.
        CHECK(m->line_h == m->size + 4);
        CHECK(m->pitch == m->line_h);
        // A page must fit the body box without touching the progress label.
        CHECK(m->lines * m->pitch + READER_LAYOUT_INSET <= READER_LAYOUT_BODY_H);
        // A half-width column is half a Hanzi; a full line must fit the body.
        CHECK(m->cols * m->size <= 2 * READER_LAYOUT_BODY_W);
        // Whole Hanzi per line, otherwise wrap width would be inconsistent.
        CHECK(m->cols % 2 == 0);
        CHECK(m->cols > 0);
        CHECK(m->lines > 0);
    }
    // The body box must line up with the reader screen: the body label starts
    // at y=68 and the 已读 label sits at y=256. If BODY_H drifts from that
    // difference the last line either overlaps the label or leaves an
    // asymmetric gap below the text.
    CHECK(68 + READER_LAYOUT_BODY_H == 256);
}

static void test_larger_text_shows_less(void)
{
    for (int i = 1; i < ROW_COUNT; ++i) {
        const reader_font_metrics_t *prev = &g_reader_font_metrics[i - 1];
        const reader_font_metrics_t *cur = &g_reader_font_metrics[i];
        CHECK(cur->lines <= prev->lines);
        CHECK(cur->cols <= prev->cols);
        CHECK(cur->pitch > prev->pitch);
    }
    // Sanity anchors: the two sizes the reader shipped with before the picker.
    // 16 px gained one line when the chapter moved into the header subtitle;
    // 20 px cannot gain a whole 24 px line without touching the 已读 label.
    CHECK(reader_layout_lines(16) == 9);
    CHECK(reader_layout_cols(16) == 24);
    CHECK(reader_layout_pitch(16) == 20);
    CHECK(reader_layout_lines(20) == 7);
    CHECK(reader_layout_cols(20) == 18);
    CHECK(reader_layout_pitch(20) == 24);
}

static void test_immersive_geometry(void)
{
    // Immersive mode exists to show MORE text, not to add blank space. Every
    // size must gain lines, fit its taller box, and — the subtle one — keep a
    // page of pure CJK inside the application's page-text buffer. If that last
    // check fails, reader_paginate() would cut the page short and bookmarks
    // would appear to land on the wrong sentence.
    for (int i = 0; i < ROW_COUNT; ++i) {
        const reader_font_metrics_t *m = &g_reader_font_metrics[i];
        CHECK(m->imm_lines > m->lines);
        CHECK(m->imm_lines * m->pitch + READER_LAYOUT_INSET <=
              READER_LAYOUT_IMMERSIVE_H);
        // +1 full-width character per line: reader_paginate lets a closing
        // punctuation mark overflow rather than starting a line with it.
        int worst_page_bytes = m->imm_lines * ((m->cols / 2 + 1) * 3 + 1);
        CHECK(worst_page_bytes <= READER_LAYOUT_MAX_PAGE_BYTES);
        CHECK(worst_page_bytes <= 1024);  // PAGE_WINDOW_SIZE in reader_app.c
        CHECK(reader_layout_immersive_lines(m->size) == m->imm_lines);
    }
    // The immersive box reaches exactly y=288, where the toast band starts:
    // that is what moves the bottom text edge down and makes the margins
    // near-symmetric. Leaving it short would waste the space again.
    CHECK(READER_LAYOUT_IMMERSIVE_TOP + READER_LAYOUT_IMMERSIVE_H == 288);
    CHECK(READER_LAYOUT_IMMERSIVE_TOP == 16);

    // Anchors: the default 16 px page grows from 9 to 13 lines, and every size
    // except 15 px gains a line. At 12 px the last line moves from y=262 to
    // y=272, leaving a bottom margin close to the 16 px top inset.
    CHECK(reader_layout_immersive_lines(16) == 13);
    CHECK(reader_layout_immersive_lines(12) == 16);
    CHECK(reader_layout_immersive_lines(20) == 11);
    CHECK(reader_layout_immersive_lines(1) == 16);   // clamps like the others
    CHECK(reader_layout_immersive_lines(200) == 11);
}

static void test_real_immersive_pagination(void)
{
    // Exercise the actual paginator at the immersive line budget. Arithmetic
    // alone cannot catch a buffer that is silently cut short: that would make
    // page.next (the byte offset a bookmark stores) disagree with what is
    // drawn, and a jump would appear to land on the wrong sentence.
    static char text[3000];  // 1000 x 3-byte Hanzi (U+4E00), no line breaks
    for (size_t i = 0; i < sizeof(text); i += 3) {
        text[i] = (char)0xE4;
        text[i + 1] = (char)0xB8;
        text[i + 2] = (char)0x80;
    }
    char out[READER_LAYOUT_MAX_PAGE_BYTES];

    for (int i = 0; i < ROW_COUNT; ++i) {
        const reader_font_metrics_t *m = &g_reader_font_metrics[i];
        reader_page_t page;
        int rc = reader_paginate(text, sizeof(text), 0, m->cols, m->imm_lines,
                                 out, sizeof(out), &page);
        CHECK(rc == 0);
        CHECK(page.length < READER_LAYOUT_MAX_PAGE_BYTES);
        CHECK(page.next > page.start);
        CHECK(page.lines <= m->imm_lines);

        // The next page must begin at exactly the byte the first one ended on.
        // That offset is what a bookmark records, in either geometry.
        reader_page_t page2;
        rc = reader_paginate(text, sizeof(text), page.next, m->cols,
                             m->imm_lines, out, sizeof(out), &page2);
        CHECK(rc == 0);
        CHECK(page2.start == page.next);
        CHECK(page2.next > page2.start);
    }
}

static void test_clamping(void)
{
    // Out-of-range requests clamp instead of reading outside the table.
    CHECK(reader_layout_metrics(1)->size == READER_FONT_MIN_SIZE);
    CHECK(reader_layout_metrics(0)->size == READER_FONT_MIN_SIZE);
    CHECK(reader_layout_metrics(-7)->size == READER_FONT_MIN_SIZE);
    CHECK(reader_layout_metrics(200)->size == READER_FONT_MAX_SIZE);

    CHECK(reader_layout_font_index(READER_FONT_MIN_SIZE) == 0);
    CHECK(reader_layout_font_index(READER_FONT_MAX_SIZE) == ROW_COUNT - 1);
    CHECK(reader_layout_font_index(16) == 4);

    // Every SELECTABLE size maps to itself, so the settings page can print the
    // value it was given and the picker can compare it to the ruler.
    for (int i = 0; i < ROW_COUNT; ++i) {
        int size = g_reader_font_sizes[i];
        CHECK(reader_layout_metrics(size)->size == size);
        CHECK(reader_layout_font_index(size) == i);
        CHECK(reader_layout_font_size(i) == size);
        CHECK(reader_layout_size_valid(size) == 1);
    }
    // The two removed sizes must be reported invalid (a stale NVS value is
    // replaced) and snap to a neighbour rather than to a missing font.
    CHECK(reader_layout_size_valid(17) == 0);
    CHECK(reader_layout_size_valid(19) == 0);
    CHECK(reader_layout_font_index(17) == 4 || reader_layout_font_index(17) == 5);
    CHECK(reader_layout_metrics(19)->size == 18 || reader_layout_metrics(19)->size == 20);
    CHECK(reader_layout_size_valid(READER_FONT_MIN_SIZE - 1) == 0);
    CHECK(reader_layout_size_valid(READER_FONT_MAX_SIZE + 1) == 0);
    // Stepping by table position is the only way to walk the ladder.
    for (int i = 0; i + 1 < ROW_COUNT; ++i) {
        int next = reader_layout_font_size(reader_layout_font_index(g_reader_font_sizes[i]) + 1);
        CHECK(next == g_reader_font_sizes[i + 1]);
    }
}

int main(void)
{
    test_table_length();
    test_rows_are_consistent();
    test_larger_text_shows_less();
    test_immersive_geometry();
    test_real_immersive_pagination();
    test_clamping();

    if (failures) {
        printf("reader_layout: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_layout: PASS\n");
    return 0;
}
