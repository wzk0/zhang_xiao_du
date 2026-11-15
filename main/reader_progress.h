// reader_progress.h — pure serialization of reading progress for NVS.
//
// The ESP32-C3 has no PSRAM and a merged-image flash can reset NVS, so the
// record is a small fixed-size blob with a magic, version and CRC32. Encoding,
// parsing, CRC and the FIFO set/find logic are independent of ESP-IDF and are
// covered by tests/test_reader_progress.c. The NVS read/write lives in
// reader_store.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define READER_PROGRESS_MAGIC     0x52445031u  // "RDP1"
#define READER_PROGRESS_VERSION   1u
#define READER_PROGRESS_MAX_BOOKS 24
#define READER_PROGRESS_ENTRY_SIZE 12u
#define READER_PROGRESS_HEADER_SIZE 16u
#define READER_PROGRESS_BLOB_SIZE \
    (READER_PROGRESS_HEADER_SIZE + READER_PROGRESS_MAX_BOOKS * READER_PROGRESS_ENTRY_SIZE)

typedef struct {
    uint32_t book_id;   // FNV-1a hash of the stable book id string
    uint32_t offset;    // byte offset of the current page
    uint16_t chapter;   // chapter index for the TOC
    uint16_t flags;     // reserved, always 0 for version 1
} reader_progress_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t crc32;
    reader_progress_entry_t entries[READER_PROGRESS_MAX_BOOKS];
} reader_progress_blob_t;

// Stable id for a book name. The uploaded filename is the identity; a hash
// keeps the fixed-size blob independent of the filename length.
uint32_t reader_book_id(const char *name);

// Standard CRC-32 (IEEE 802.3), computed in pure C.
uint32_t reader_crc32(const void *data, size_t len);

void reader_progress_init(reader_progress_blob_t *blob);

// Serialize into `out` (little-endian) and return the byte count, or -1 on
// invalid arguments. The CRC covers magic/version/count and all stored entries.
int reader_progress_serialize(const reader_progress_blob_t *blob,
                              uint8_t *out, size_t out_size);

// Parse and validate a blob. Returns 0 on success, -1 on any corruption,
// truncation, version mismatch or out-of-range count. On failure `out` is
// reset to a fresh empty record so callers can safely continue with defaults.
int reader_progress_parse(const uint8_t *buf, size_t len,
                          reader_progress_blob_t *out);

// 1 and copies the entry when found, 0 otherwise.
int reader_progress_find(const reader_progress_blob_t *blob, uint32_t book_id,
                         reader_progress_entry_t *out);

// Insert or update an entry. When the table is full the oldest entry is dropped
// (FIFO) so the book currently being read is always retained.
void reader_progress_set(reader_progress_blob_t *blob,
                         const reader_progress_entry_t *entry);

// Drop the progress record for a deleted book. Returns 1 when an entry was
// removed, 0 when the book had no record.
int reader_progress_remove(reader_progress_blob_t *blob, uint32_t book_id);
