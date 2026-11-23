// reader_pagemap.c — see reader_pagemap.h. Pure logic only.
#include "reader_pagemap.h"

#include <stddef.h>

void reader_pagemap_init(reader_pagemap_t *map, uint32_t *anchors, uint32_t capacity)
{
    if (!map) return;
    map->anchors = anchors;
    map->capacity = anchors ? capacity : 0;
    map->groups = 0;
    map->pages = 0;
    map->truncated = 0;
}

void reader_pagemap_build(reader_pagemap_t *map, reader_pagemap_next_fn next, void *ctx,
                          uint32_t book_size, reader_pagemap_progress_fn progress,
                          void *progress_ctx)
{
    if (!map) return;
    map->groups = 0;
    map->pages = 0;
    map->truncated = 0;
    if (!next || book_size == 0) return;

    uint32_t cur = 0;
    for (;;) {
        // `cur` starts page (pages + 1). Anchor every GROUP pages, starting at
        // page 1 (offset 0), which is what page_at() searches.
        const uint32_t page = map->pages + 1;
        if ((page - 1u) % READER_PAGEMAP_GROUP == 0) {
            if (map->anchors && map->groups < map->capacity) {
                map->anchors[map->groups++] = cur;
            } else {
                map->truncated = 1;
            }
        }
        map->pages = page;

        uint32_t nxt = 0;
        // A page must make progress; a paginator that returns the same offset
        // twice would otherwise spin here forever.
        if (!next(ctx, cur, &nxt) || nxt <= cur || nxt > book_size) break;
        cur = nxt;
        if (progress) progress(progress_ctx, cur, book_size);
    }
}

uint32_t reader_pagemap_page_at(const reader_pagemap_t *map, reader_pagemap_next_fn next,
                                void *ctx, uint32_t offset)
{
    if (!map || !map->anchors || map->groups == 0 || !next) return 0;

    // First anchor AFTER `offset`; the one before it is the closest page start
    // at or below the requested byte.
    uint32_t lo = 0, hi = map->groups;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (map->anchors[mid] <= offset) lo = mid + 1;
        else hi = mid;
    }
    uint32_t group = lo ? lo - 1 : 0;
    uint32_t cur = map->anchors[group];
    uint32_t page = group * READER_PAGEMAP_GROUP + 1;

    // Step forward only while the NEXT page also starts at or before `offset`:
    // the page containing `offset` is the last one whose start is <= offset.
    // At most GROUP-1 steps; the guard only protects against a broken paginator.
    for (uint32_t guard = 0; guard < READER_PAGEMAP_GROUP; ++guard) {
        uint32_t nxt = 0;
        if (!next(ctx, cur, &nxt) || nxt <= cur) break;
        if (nxt > offset) break;
        cur = nxt;
        ++page;
    }
    return page;
}

int reader_pagemap_offset_of(const reader_pagemap_t *map, reader_pagemap_next_fn next,
                             void *ctx, uint32_t page, uint32_t *offset)
{
    if (!map || !map->anchors || map->groups == 0 || !next || !offset) return 0;
    if (page == 0 || page > map->pages) return 0;

    uint32_t group = (page - 1u) / READER_PAGEMAP_GROUP;
    if (group >= map->groups) return 0;
    uint32_t cur = map->anchors[group];
    uint32_t cur_page = group * READER_PAGEMAP_GROUP + 1;
    while (cur_page < page) {
        uint32_t nxt = 0;
        if (!next(ctx, cur, &nxt) || nxt <= cur) return 0;
        cur = nxt;
        ++cur_page;
    }
    *offset = cur;
    return 1;
}