// Host tests for the page map that feeds the reader footer ("1/30") and the TOC
// page tab ("1—10页"). The map itself never touches the file system or LVGL, so
// the grouping and the binary search can be pinned here.
// Build: cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_reader_pagemap.c
//        main/reader_pagemap.c -o /tmp/test_reader_pagemap
#include "reader_pagemap.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

// Synthetic book: fixed page length, so every expectation is obvious.
typedef struct {
    uint32_t book_size;
    uint32_t page_bytes;
} fixed_ctx_t;

static int fixed_next(void *ctx, uint32_t offset, uint32_t *next)
{
    fixed_ctx_t *c = ctx;
    if (offset + c->page_bytes >= c->book_size) {
        // Last page: nothing follows. (A page may be short at the end.)
        return 0;
    }
    *next = offset + c->page_bytes;
    return 1;
}

// Ragged book: page lengths cycle 40/80/120 bytes to prove the anchors, not a
// fixed stride, drive the search. The walker is stateless (it looks the offset
// up in a precomputed table), because the map may be re-walked from an anchor.
#define RAGGED_MAX 256
static uint32_t g_ragged_starts[RAGGED_MAX];
static uint32_t g_ragged_count;

static void ragged_prepare(uint32_t book_size)
{
    static const uint32_t steps[] = { 40, 80, 120 };
    uint32_t cur = 0;
    g_ragged_count = 0;
    for (;;) {
        g_ragged_starts[g_ragged_count++] = cur;
        uint32_t step = steps[(g_ragged_count - 1) % 3];
        if (cur + step >= book_size) break;
        cur += step;
        if (g_ragged_count >= RAGGED_MAX) break;
    }
}

static int ragged_next(void *ctx, uint32_t offset, uint32_t *next)
{
    (void)ctx;
    for (uint32_t i = 0; i + 1 < g_ragged_count; ++i) {
        if (g_ragged_starts[i] == offset) {
            *next = g_ragged_starts[i + 1];
            return 1;
        }
    }
    return 0;
}

static void test_fixed_pages(void)
{
    static uint32_t anchors[64];
    reader_pagemap_t map;
    fixed_ctx_t ctx = { .book_size = 1000, .page_bytes = 100 };
    reader_pagemap_init(&map, anchors, 64);
    reader_pagemap_build(&map, fixed_next, &ctx, ctx.book_size, NULL, NULL);

    CHECK(map.pages == 10);
    CHECK(map.groups == 1);            // page 1 only: the next anchor is page 11
    CHECK(map.truncated == 0);
    CHECK(map.anchors[0] == 0);

    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 0) == 1);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 99) == 1);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 100) == 2);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 500) == 6);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 999) == 10);

    uint32_t offset = 0xFFFFFFFFu;
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 1, &offset) == 1 && offset == 0);
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 7, &offset) == 1 && offset == 600);
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 10, &offset) == 1 && offset == 900);
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 11, &offset) == 0);
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 0, &offset) == 0);
}

static void test_many_groups(void)
{
    static uint32_t anchors[128];
    reader_pagemap_t map;
    fixed_ctx_t ctx = { .book_size = 100000, .page_bytes = 100 };  // 1000 pages
    reader_pagemap_init(&map, anchors, 128);
    reader_pagemap_build(&map, fixed_next, &ctx, ctx.book_size, NULL, NULL);

    CHECK(map.pages == 1000);
    CHECK(map.groups == 100);
    CHECK(map.truncated == 0);

    // Every anchor is the first byte of page (index * GROUP + 1).
    for (uint32_t g = 0; g < map.groups; ++g) {
        CHECK(map.anchors[g] == g * READER_PAGEMAP_GROUP * 100);
    }
    // Page lookup on both sides of an anchor boundary.
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 0) == 1);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 900) == 10);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 1000) == 11);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 99900) == 1000);
    // The page tab jumps to the first page of a range, which must round-trip.
    for (uint32_t g = 0; g < map.groups; ++g) {
        uint32_t page = g * READER_PAGEMAP_GROUP + 1;
        uint32_t offset = 0;
        CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, page, &offset) == 1);
        CHECK(offset == map.anchors[g]);
        CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, offset) == page);
    }
}

static void test_truncated_keeps_the_count(void)
{
    static uint32_t anchors[3];
    reader_pagemap_t map;
    fixed_ctx_t ctx = { .book_size = 100000, .page_bytes = 100 };  // 1000 pages
    reader_pagemap_init(&map, anchors, 3);
    reader_pagemap_build(&map, fixed_next, &ctx, ctx.book_size, NULL, NULL);

    // The count stays exact, so the footer still reports the real total; only
    // the indexed range stops.
    CHECK(map.pages == 1000);
    CHECK(map.groups == 3);
    CHECK(map.truncated == 1);
    uint32_t offset = 0;
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 21, &offset) == 1);
    CHECK(offset == 2000);
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 31, &offset) == 0);
}

static void test_no_anchor_storage(void)
{
    reader_pagemap_t map;
    fixed_ctx_t ctx = { .book_size = 1000, .page_bytes = 100 };
    reader_pagemap_init(&map, NULL, 0);
    reader_pagemap_build(&map, fixed_next, &ctx, ctx.book_size, NULL, NULL);
    CHECK(map.pages == 10);
    CHECK(map.groups == 0);
    CHECK(map.truncated == 1);
    // Without anchors nothing may be reported as a page number.
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 500) == 0);
    uint32_t offset = 0;
    CHECK(reader_pagemap_offset_of(&map, fixed_next, &ctx, 5, &offset) == 0);
}

static void test_ragged_pages(void)
{
    static uint32_t anchors[64];
    reader_pagemap_t map;
    fixed_ctx_t ctx = { .book_size = 6000, .page_bytes = 0 };
    ragged_prepare(ctx.book_size);

    reader_pagemap_init(&map, anchors, 64);
    reader_pagemap_build(&map, ragged_next, &ctx, ctx.book_size, NULL, NULL);
    CHECK(map.pages == g_ragged_count);

    for (uint32_t page = 1; page <= g_ragged_count; ++page) {
        uint32_t offset = 0;
        if (page <= map.groups * READER_PAGEMAP_GROUP) {
            CHECK(reader_pagemap_offset_of(&map, ragged_next, &ctx, page, &offset) == 1);
            CHECK(offset == g_ragged_starts[page - 1]);
        }
        // Every page's own first byte must report that page.
        CHECK(reader_pagemap_page_at(&map, ragged_next, &ctx, g_ragged_starts[page - 1]) == page);
    }
}

static void test_empty_book(void)
{
    static uint32_t anchors[4];
    reader_pagemap_t map;
    fixed_ctx_t ctx = { .book_size = 0, .page_bytes = 100 };
    reader_pagemap_init(&map, anchors, 4);
    reader_pagemap_build(&map, fixed_next, &ctx, 0, NULL, NULL);
    CHECK(map.pages == 0);
    CHECK(map.groups == 0);
    CHECK(reader_pagemap_page_at(&map, fixed_next, &ctx, 0) == 0);
}

typedef struct {
    uint32_t last_done;
    uint32_t last_total;
    uint32_t calls;
} progress_ctx_t;

static void progress_cb(void *ctx, uint32_t done, uint32_t total)
{
    progress_ctx_t *p = ctx;
    p->last_done = done;
    p->last_total = total;
    ++p->calls;
}

static void test_progress_reports_offsets(void)
{
    // The overlay bar is driven by byte offsets, so the last report must be the
    // end of the book and every report must carry the same total.
    static uint32_t anchors[8];
    reader_pagemap_t map;
    fixed_ctx_t ctx = { .book_size = 1000, .page_bytes = 100 };
    progress_ctx_t seen = { 0, 0, 0 };
    reader_pagemap_init(&map, anchors, 8);
    reader_pagemap_build(&map, fixed_next, &ctx, ctx.book_size, progress_cb, &seen);

    CHECK(map.pages == 10);
    CHECK(seen.calls == 9);              // one report per page after the first
    CHECK(seen.last_total == ctx.book_size);
    CHECK(seen.last_done == 900);        // start of the last page
}

int main(void)
{
    test_fixed_pages();
    test_many_groups();
    test_truncated_keeps_the_count();
    test_no_anchor_storage();
    test_ragged_pages();
    test_empty_book();
    test_progress_reports_offsets();

    if (failures) {
        printf("reader_pagemap: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_pagemap: PASS\n");
    return 0;
}