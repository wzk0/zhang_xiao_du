// reader_bookmark.h — pure bookmark blob codec.
//
// Mirrors reader_progress: a fixed-size blob with magic, version and CRC32 so a
// merged-image flash that wipes NVS degrades to "no bookmarks" instead of a
// half-valid table. Book identity is the same FNV-1a hash used for progress, so
// the record stays independent of the (variable-length) filename. Encoding,
// parsing, de-duplication, FIFO eviction and removal are ESP-IDF free and are
// covered by tests/test_reader_bookmark.c; the NVS read/write lives in
// reader_store.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define READER_BOOKMARK_MAGIC       0x52424D31u  // "RBM1"
#define READER_BOOKMARK_VERSION     1u
#define READER_BOOKMARK_MAX         32
#define READER_BOOKMARK_ENTRY_SIZE  12u
#define READER_BOOKMARK_HEADER_SIZE 16u
#define READER_BOOKMARK_BLOB_SIZE \
    (READER_BOOKMARK_HEADER_SIZE + READER_BOOKMARK_MAX * READER_BOOKMARK_ENTRY_SIZE)

typedef struct {
    uint32_t book_id;   // FNV-1a hash of the stable book id string
    uint32_t offset;    // byte offset of the bookmarked page
    uint16_t chapter;   // chapter index at that offset
    uint16_t flags;     // reserved, always 0 for version 1
} reader_bookmark_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t crc32;
    reader_bookmark_entry_t entries[READER_BOOKMARK_MAX];
} reader_bookmark_blob_t;

void reader_bookmark_init(reader_bookmark_blob_t *blob);

// Serialize into `out` (little-endian) and return the byte count, or -1 on
// invalid arguments. The CRC covers magic/version/count and all stored entries.
int reader_bookmark_serialize(const reader_bookmark_blob_t *blob,
                              uint8_t *out, size_t out_size);

// Parse and validate a blob. Returns 0 on success, -1 on corruption, truncation
// or version mismatch. On failure `out` is reset to a fresh empty record.
int reader_bookmark_parse(const uint8_t *buf, size_t len,
                          reader_bookmark_blob_t *out);

// Index of (book_id, offset) or -1 when absent.
int reader_bookmark_find(const reader_bookmark_blob_t *blob, uint32_t book_id,
                         uint32_t offset);

// Append a bookmark. Returns 1 when inserted, 0 when the same page is already
// bookmarked, -1 on invalid arguments. When full the oldest entry is dropped.
int reader_bookmark_add(reader_bookmark_blob_t *blob,
                        const reader_bookmark_entry_t *entry);

// Remove a bookmark. Returns 1 when removed, 0 when it was not present.
int reader_bookmark_remove(reader_bookmark_blob_t *blob, uint32_t book_id,
                           uint32_t offset);

// Copy this book's bookmarks in ascending page order. Returns the count.
int reader_bookmark_list(const reader_bookmark_blob_t *blob, uint32_t book_id,
                         reader_bookmark_entry_t *out, int max);

// Drop every bookmark belonging to a deleted book. Returns the number removed.
int reader_bookmark_remove_book(reader_bookmark_blob_t *blob, uint32_t book_id);
