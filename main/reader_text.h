// reader_text.h — pure text engine for the eBook reader.
//
// Everything in this file is independent of ESP-IDF and LVGL so pagination,
// UTF-8 handling, chapter detection and character-set accounting can be covered
// by host tests (tests/test_reader_text.c). The device has no PSRAM, so books are
// read in small windows; pagination always works on a bounded, non-NUL-terminated
// window and returns byte offsets relative to that window.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define READER_INVALID_CP 0xFFFFFFFFu

typedef struct {
    size_t start;   // byte offset (relative to the window) where the page starts
    size_t next;    // byte offset of the next page; > start guarantees progress
    size_t length;  // bytes written to the output buffer, excluding the NUL
    int lines;      // rendered line count
    int is_last;    // 1 when the window was consumed entirely
} reader_page_t;

typedef struct {
    uint32_t total;        // non-ASCII, non-control code points examined
    uint32_t unsupported;  // code points outside the declared font inventory
    uint32_t sample[8];    // first unsupported code points, for the UI hint
    uint32_t sample_count;
} reader_charset_report_t;

// Decode one UTF-8 code point. On malformed input it consumes one byte and
// returns READER_INVALID_CP so callers can substitute a visible '?' instead of
// splitting a multibyte sequence (which would corrupt the whole page).
uint32_t reader_utf8_decode(const char *s, size_t len, size_t *consumed);

// 1 for full-width code points (CJK, full-width punctuation, Hangul, ...),
// 0 for half-width. Pagination counts 2 units for wide and 1 for narrow.
int reader_is_wide(uint32_t cp);

// Lay out one page. `cols_units` is the maximum line width in half-width units;
// `max_lines` is the page's line budget. The output is NUL-terminated UTF-8 with
// explicit '\n' between lines. Returns 0 on success, -1 on invalid arguments.
// Chinese line-break rules keep closing punctuation off the start of a line by
// allowing a one-character overflow instead (a line's width budget already has
// one character of slack).
int reader_paginate(const char *text, size_t text_len, size_t start,
                    int cols_units, int max_lines,
                    char *out, size_t out_size, reader_page_t *page);

// Apply the same pagination policy with a hard display-width limit. Printable
// ASCII advances are in 1/16 px; other characters use one em conservatively.
int reader_paginate_fit(const char *text, size_t text_len, size_t start,
                        int cols_units, int max_lines, int font_px, int width_px,
                        const uint16_t *ascii_advance,
                        char *out, size_t out_size, reader_page_t *page);

// Build a short, single-line label from the text at a bookmark offset.
// Leading whitespace and line breaks are skipped; a hard line break ends the
// excerpt; UTF-8 sequences are never split. `max_units` counts half-width
// units (a Hanzi costs 2), so a caller can match the label's display width.
// When text remains after the cut, a trailing ellipsis is appended. Returns
// the number of bytes written (0 when there is no printable text), or -1 on
// invalid arguments.
int reader_excerpt(const char *text, size_t text_len, int max_units,
                   char *out, size_t out_size);

// 1 when a line looks like a chapter heading. Used by the streaming index
// builder in reader_store.c. `len` is the line length without the newline.
int reader_chapter_match(const char *line, size_t len);

// Count code points outside the declared font inventory. `is_supported` is
// supplied by the application (it consults the generated font), keeping this
// file free of LVGL. ASCII and control characters are always treated as covered.
int reader_scan_charset(const char *text, size_t text_len,
                        int (*is_supported)(uint32_t cp, void *ctx), void *ctx,
                        reader_charset_report_t *report);
