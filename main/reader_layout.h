// reader_layout.h — pure layout metrics shared by pagination and the UI.
//
// Keeping these independent of ESP-IDF/LVGL lets host tests assert that every
// selectable font size still fits the fixed body box. A CJK font's line_height
// is NOT its pixel size: every generated font reports size + 4 (see
// docs/reference/shinku-chen/cjk-line-pitch-from-font-metrics.md), so the line
// pitch fed to the paginator is always line_height, never the pixel size.
#pragma once

#include <stdint.h>

// On the 240x320 screen the reading body runs from y=68 (below the header:
// book name at y=14, chapter subtitle at y=38) down to y=256 (the 已读 label):
// 188 px of real, non-overlapping height. The chapter moved up into the header
// subtitle last round, which freed the space above the body; we spend it on one
// extra line and keep the gap above the text close to the gap below it, so the
// block reads as vertically centred. Progress bar + key hints occupy 276..310.
// This is the budget the assertions in reader_layout.c enforce. The body is
// 204 px wide (240 minus two 16 px margins plus the trailing punctation slack
// counted by READER_LAYOUT_INSET).
#define READER_LAYOUT_BODY_W   204
#define READER_LAYOUT_BODY_H   188

// Immersive pages drop the header, chapter, progress and key hints, so the text
// may use almost the whole panel. A 16 px top inset and a bottom edge at y=288
// (exactly where the transient bookmark toast starts) let even the default
// 16 px font gain a line: the last line moves from y=262 down to y=276..280,
// so the previously large bottom blank nearly matches the top inset. The
// renderer draws the same line count the paginator uses, so bookmarks never
// land off by a line.
#define READER_LAYOUT_IMMERSIVE_TOP 16
#define READER_LAYOUT_IMMERSIVE_H  272

// Upper bound on the bytes the paginator may emit for ONE page. The worst case
// is an immersive 12 px page: 16 lines of 16 full-width CJK characters (3 bytes
// each) plus one '\n' and one overflow punctuation slot per line. The
// application's page-text buffer and its read window must both cover this;
// reader_layout.c asserts it per size.
#define READER_LAYOUT_MAX_PAGE_BYTES 896

// Half-width units of slack kept free at the right edge so a closing quote or
// comma is never clipped by the label box.
#define READER_LAYOUT_INSET      8

// Selectable body sizes. 12 px is the smallest size at which a 2 bit-per-pixel
// CJK glyph is still legible on this panel; 20 px is the largest that still
// leaves seven full lines on screen.
#define READER_FONT_MIN_SIZE     12
#define READER_FONT_MAX_SIZE     20
#define READER_FONT_DEFAULT_SIZE 16

typedef struct {
    uint8_t size;       // glyph pixel size handed to reader_font()
    uint8_t line_h;     // font->line_height as emitted by the generator
    uint8_t pitch;      // distance between baselines; always == line_h
    uint8_t cols;       // half-width units per line (2 units per Hanzi)
    uint8_t lines;      // full lines on a normal page
    uint8_t imm_lines;  // full lines on an immersive (full-height) page
} reader_font_metrics_t;

// Table indexed by (size - READER_FONT_MIN_SIZE); one row per selectable size.
extern const reader_font_metrics_t
    g_reader_font_metrics[READER_FONT_MAX_SIZE - READER_FONT_MIN_SIZE + 1];

// Clamps any requested size into the supported range and returns its row index.
int reader_layout_font_index(int size);

// Metrics for a requested size. Never NULL: out-of-range sizes are clamped.
const reader_font_metrics_t *reader_layout_metrics(int size);

int reader_layout_cols(int size);
int reader_layout_lines(int size);
int reader_layout_pitch(int size);

// Lines per immersive page. The paginator and the reader screen MUST both use
// this when immersive mode is on: a mismatch would clip text and make every
// bookmark appear to jump to the wrong place.
int reader_layout_immersive_lines(int size);

// True when `size` is a selectable size (used by the settings store loader).
int reader_layout_size_valid(int size);
