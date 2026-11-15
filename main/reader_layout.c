// reader_layout.c — the single source of truth for per-size page geometry.
//
// One row per selectable body size. Everything the paginator needs (columns in
// half-width units, lines per page) and everything the reader screen needs
// (line pitch) comes from here, so the preview in the font picker and the real
// page can never disagree. Immersive mode has its own line budget: it removes
// the chrome, so the freed vertical space becomes MORE TEXT, not more blank.
#include "reader_layout.h"

#define READER_FONT_COUNT (READER_FONT_MAX_SIZE - READER_FONT_MIN_SIZE + 1)

// size, line_height (= size + 4), cols, normal lines, immersive lines.
// cols = 2 * floor((BODY_W - INSET) / size) rounded down to a whole Hanzi.
// lines = floor((BODY_H - INSET) / line_height).
// imm_lines = floor((IMMERSIVE_H - INSET) / line_height).
const reader_font_metrics_t g_reader_font_metrics[READER_FONT_COUNT] = {
    { .size = 12, .line_h = 16, .pitch = 16, .cols = 32, .lines = 11, .imm_lines = 16 },
    { .size = 13, .line_h = 17, .pitch = 17, .cols = 30, .lines = 10, .imm_lines = 15 },
    { .size = 14, .line_h = 18, .pitch = 18, .cols = 28, .lines = 10, .imm_lines = 14 },
    { .size = 15, .line_h = 19, .pitch = 19, .cols = 26, .lines =  9, .imm_lines = 13 },
    { .size = 16, .line_h = 20, .pitch = 20, .cols = 24, .lines =  9, .imm_lines = 13 },
    { .size = 17, .line_h = 21, .pitch = 21, .cols = 22, .lines =  8, .imm_lines = 12 },
    { .size = 18, .line_h = 22, .pitch = 22, .cols = 20, .lines =  8, .imm_lines = 12 },
    { .size = 19, .line_h = 23, .pitch = 23, .cols = 20, .lines =  7, .imm_lines = 11 },
    { .size = 20, .line_h = 24, .pitch = 24, .cols = 18, .lines =  7, .imm_lines = 11 },
};

_Static_assert(sizeof(g_reader_font_metrics) / sizeof(g_reader_font_metrics[0]) ==
                   READER_FONT_COUNT,
               "one metrics row per selectable size");

// Every row must fit its body box, and the immersive box must leave room for
// the toast band at the bottom of the 240x320 panel. The last assertion is the
// important one for immersive mode: a page full of Hanzi must fit the app's
// page-text buffer, otherwise the paginator would silently cut the page short.
#define READER_LAYOUT_CHECK(size, pitch, cols, lines, imm_lines)                 \
    _Static_assert((pitch) == (size) + 4,                                      \
                   "pitch must equal the generated line_height at " #size "px"); \
    _Static_assert((lines) * (pitch) + READER_LAYOUT_INSET <= READER_LAYOUT_BODY_H, \
                   "normal page must fit the body height at " #size "px");     \
    _Static_assert((cols) * (size) <= 2 * READER_LAYOUT_BODY_W,               \
                   "line must fit the body width at " #size "px");             \
    _Static_assert((imm_lines) > (lines),                                     \
                   "immersive page must show more lines at " #size "px");      \
    _Static_assert((imm_lines) * (pitch) + READER_LAYOUT_INSET <=              \
                       READER_LAYOUT_IMMERSIVE_H,                              \
                   "immersive page must fit its box at " #size "px");          \
    _Static_assert((imm_lines) * (((cols) / 2 + 1) * 3 + 1) <=                \
                       READER_LAYOUT_MAX_PAGE_BYTES,                           \
                   "immersive page may overflow the text buffer at " #size "px")

READER_LAYOUT_CHECK(12, 16, 32, 11, 16);
READER_LAYOUT_CHECK(13, 17, 30, 10, 15);
READER_LAYOUT_CHECK(14, 18, 28, 10, 14);
READER_LAYOUT_CHECK(15, 19, 26, 9, 13);
READER_LAYOUT_CHECK(16, 20, 24, 9, 13);
READER_LAYOUT_CHECK(17, 21, 22, 8, 12);
READER_LAYOUT_CHECK(18, 22, 20, 8, 12);
READER_LAYOUT_CHECK(19, 23, 20, 7, 11);
READER_LAYOUT_CHECK(20, 24, 18, 7, 11);

// The immersive text block plus its top inset must stay above the footer band
// (UI_H = 320 in reader_ui.c; 288 is where the key pills start).
_Static_assert(READER_LAYOUT_IMMERSIVE_TOP + READER_LAYOUT_IMMERSIVE_H <= 320,
               "immersive text box must fit the 320 px panel");
_Static_assert(READER_LAYOUT_IMMERSIVE_TOP + READER_LAYOUT_IMMERSIVE_H <= 288,
               "immersive text must not overlap the toast band");

int reader_layout_font_index(int size)
{
    if (size <= READER_FONT_MIN_SIZE) return 0;
    if (size >= READER_FONT_MAX_SIZE) return READER_FONT_COUNT - 1;
    return size - READER_FONT_MIN_SIZE;
}

int reader_layout_size_valid(int size)
{
    return size >= READER_FONT_MIN_SIZE && size <= READER_FONT_MAX_SIZE;
}

const reader_font_metrics_t *reader_layout_metrics(int size)
{
    return &g_reader_font_metrics[reader_layout_font_index(size)];
}

int reader_layout_cols(int size)
{
    return reader_layout_metrics(size)->cols;
}

int reader_layout_lines(int size)
{
    return reader_layout_metrics(size)->lines;
}

int reader_layout_pitch(int size)
{
    return reader_layout_metrics(size)->pitch;
}

int reader_layout_immersive_lines(int size)
{
    return reader_layout_metrics(size)->imm_lines;
}
