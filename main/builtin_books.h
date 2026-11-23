// builtin_books.h — the guide and one full-length novel compiled into flash.
//
// They exist so the reader has readable content on a blank device (a merged
// 0x0 flash clears the books partition). Uploaded books live in the
// "books" partition; see reader_store.c.
//
// The bodies are stored as zdx-compressed images (builtin_books_data.c) to
// save flash; reader_store.c decodes them with reader_zdx_open_mem(). Their
// canonical UTF-8 text lives in assets/books/*.txt and is scanned by the font
// generator, so no glyph can be missing here.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "reader_zdx.h"

typedef struct {
    const char *id;      // stable identity used for NVS progress
    const char *title;   // Chinese display title
    const unsigned char *zdx;
    size_t zdx_len;
    uint32_t orig_size;  // uncompressed UTF-8 byte length (offset space)
    uint32_t text_chars; // non-whitespace code points (cover subtitle)
} reader_builtin_book_t;

// The guide documents the device itself and is the ONLY built-in that cannot
// be removed. The single sample novel may be deleted from the shelf;
// reader_store.c remembers that choice in NVS as a bit mask, so the array must
// stay within 8 entries (index 0 is the guide, bit 1 covers the novel).
// The rewritten guide must open at its new cover; old byte offsets no longer match.
#define READER_BUILTIN_GUIDE_ID "builtin:guide:v2"

extern const reader_builtin_book_t g_builtin_books[];
extern const int g_builtin_book_count;

// Open a decoder for a built-in book's zdx image. Returns 0 on success.
// The caller owns `z` and must not close an fp (the source is flash memory).
int reader_builtin_open(const reader_builtin_book_t *book, reader_zdx_t *z);
