// reader_pagemap.h — page numbering for the reader footer and the TOC page tab.
//
// Pages are a pure function of the layout geometry (font size, immersive mode,
// column width), so pagination has no closed form: the only way to know how
// many pages a book has, or which page a byte offset falls on, is to walk it
// with the paginator that will draw it. This module owns the bookkeeping for
// that walk and nothing else: it never touches ESP-IDF, LVGL or the file
// system, so the arithmetic is covered by host tests.
//
// One anchor is stored per READER_PAGEMAP_GROUP pages. That is enough for every
// feature that needs pages:
//   * total page count      — counted, exact;
//   * "which page am I on"  — binary search + at most GROUP-1 page steps;
//   * "jump to page N"      — the same, so the TOC page list is O(GROUP) work.
// Ten pages per anchor is also exactly the TOC page-tab step, so no second
// index is needed.
#pragma once

#include <stdint.h>

// Pages per stored anchor, and the step of the TOC page tab ("1—10页").
#define READER_PAGEMAP_GROUP 10u

typedef struct {
    uint32_t *anchors;   // caller-owned array: byte offset of page (i*GROUP + 1)
    uint32_t capacity;   // anchors the array can hold
    uint32_t groups;     // anchors actually stored
    uint32_t pages;      // total pages in the book (exact, even when truncated)
    uint8_t truncated;   // ran out of anchor slots; late pages are not indexed
} reader_pagemap_t;

// Advance one page: `offset` is where a page starts, `*next` receives where the
// following page starts. Returns 1 on success, 0 when `offset` is the last page
// (or the page cannot be laid out).
typedef int (*reader_pagemap_next_fn)(void *ctx, uint32_t offset, uint32_t *next);

// `done`/`total` are ORIGINAL byte offsets, so the caller can draw a bar.
typedef void (*reader_pagemap_progress_fn)(void *ctx, uint32_t done, uint32_t total);

// Prepare `map` over a caller-owned array (NULL/0 is allowed: only the page
// count is then collected).
void reader_pagemap_init(reader_pagemap_t *map, uint32_t *anchors, uint32_t capacity);

// Walk the whole book once. Always fills `pages`; fills `anchors`/`groups` when
// the array is big enough, otherwise sets `truncated`.
void reader_pagemap_build(reader_pagemap_t *map, reader_pagemap_next_fn next, void *ctx,
                          uint32_t book_size, reader_pagemap_progress_fn progress,
                          void *progress_ctx);

// 1-based page containing `offset`, or 0 when the map holds nothing.
uint32_t reader_pagemap_page_at(const reader_pagemap_t *map, reader_pagemap_next_fn next,
                                void *ctx, uint32_t offset);

// Byte offset where 1-based `page` starts. Returns 1 on success, 0 when the page
// is outside the indexed range.
int reader_pagemap_offset_of(const reader_pagemap_t *map, reader_pagemap_next_fn next,
                             void *ctx, uint32_t page, uint32_t *offset);