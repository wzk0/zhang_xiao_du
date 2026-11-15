// builtin_books.h — the guide and one full-length novel compiled into flash.
//
// They exist so the reader has readable content on a blank device (a merged
// 0x0 flash clears the books partition). Uploaded books live in the SPIFFS
// "books" partition; see reader_store.c.
#pragma once

#include <stdint.h>

typedef struct {
    const char *id;      // stable identity used for NVS progress
    const char *title;   // Chinese display title
    const char *text;    // NUL-terminated UTF-8
} reader_builtin_book_t;

// The guide documents the device itself and is the ONLY built-in that cannot
// be removed. The single sample novel may be deleted from the shelf;
// reader_store.c remembers that choice in NVS as a bit mask, so the array must
// stay within 8 entries (index 0 is the guide, bit 1 covers the novel).
#define READER_BUILTIN_GUIDE_ID "builtin:guide"

extern const reader_builtin_book_t g_builtin_books[];
extern const int g_builtin_book_count;
