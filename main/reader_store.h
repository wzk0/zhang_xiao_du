// reader_store.h — books in the SPIFFS "books" partition and progress in NVS.
//
// Built-in sample books are compiled into Flash; uploaded books are files under
// /books. The board has no PSRAM (largest free block is only a few KiB), so all
// reading is streaming: reader_store_read() returns a bounded window and the
// pure pagination engine works on that window.
#pragma once

#include "esp_err.h"
#include "reader_bookmark.h"
#include "reader_text.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define READER_BOOK_ID_MAX      48
#define READER_BOOK_TITLE_MAX   48
#define READER_MAX_BOOKS        24
#define READER_MAX_CHAPTERS     64
#define READER_CHAPTER_TITLE_MAX 40
#define READER_UPLOAD_TMP_NAME  ".upload.tmp"
#define READER_PAGE_HISTORY_MAX 64

typedef struct {
    char id[READER_BOOK_ID_MAX];
    char title[READER_BOOK_TITLE_MAX];
    uint32_t size;       // bytes stored in flash (compressed zdx footprint)
    uint32_t orig_size;  // original UTF-8 byte length (offset/progress space)
    bool builtin;
} reader_book_info_t;

typedef struct {
    size_t offset;
    char title[READER_CHAPTER_TITLE_MAX];
} reader_chapter_info_t;

typedef struct {
    reader_chapter_info_t items[READER_MAX_CHAPTERS];
    int count;
    // Non-whitespace UTF-8 code points in the whole book, counted during the
    // same streaming pass that finds chapters (no extra flash read). Shown on
    // the title card as "N字 · X KB".
    size_t char_count;
} reader_book_index_t;

// Mounts SPIFFS (formatting a blank partition on first boot) and opens NVS.
// nvs_flash_init() must already have succeeded. Safe to call once at boot.
esp_err_t reader_store_init(void);

// Visible built-in books first (deleted samples are filtered out), then
// uploaded files. Returns the number of books.
int reader_store_list(reader_book_info_t *out, int max);

// Remaining storage space, in bytes. Used by the transfer page and web page.
esp_err_t reader_store_space(size_t *total, size_t *used, size_t *free_bytes);

// Stream up to `len` bytes from `offset`. Returns bytes read (0 at EOF/error).
int reader_store_read(const char *id, size_t offset, char *buf, size_t len);

// Scan a whole book (streaming) for chapter headings and, in the same pass,
// count its non-whitespace UTF-8 code points into out->char_count.
typedef void (*reader_index_progress_fn)(size_t done, size_t total, void *ctx);
esp_err_t reader_store_build_index(const char *id, reader_book_index_t *out,
                                   reader_index_progress_fn progress, void *ctx);

// Best-effort cache for the exact paginated history at a saved byte offset.
int reader_store_history_load(const char *id, uint32_t offset, uint32_t layout,
                              uint32_t history[READER_PAGE_HISTORY_MAX]);
void reader_store_history_save(const char *id, uint32_t offset, uint32_t layout,
                               const uint32_t *history, int count);

// Upload lifecycle. Only one upload may be in flight at a time.
esp_err_t reader_store_begin_upload(const char *name, char *out_id, size_t out_id_size);
esp_err_t reader_store_write_chunk(const char *data, size_t len);
esp_err_t reader_store_finish_upload(const char *id, reader_charset_report_t *coverage_out);
void reader_store_abort_upload(void);
esp_err_t reader_store_delete(const char *id);

// Reading progress and UI settings, persisted with magic/version/CRC32.
uint32_t reader_store_get_offset(const char *id, uint16_t *chapter_out);
void reader_store_set_offset(const char *id, uint32_t offset, uint16_t chapter);
// True when this book already has a saved progress record. A first open has
// none, which is what gates the title card; offset 0 alone is not enough
// because the very first page also saves offset 0.
bool reader_store_has_progress(const char *id);
uint8_t reader_store_get_setting(const char *key, uint8_t fallback);
void reader_store_set_setting(const char *key, uint8_t value);

// Bookmarks for one book, in ascending page order. book_id is
// reader_book_id(name). The table is shared across books (READER_BOOKMARK_MAX)
// and persisted in NVS next to the reading progress.
int reader_store_bookmark_list(uint32_t book_id, reader_bookmark_entry_t *out,
                               int max);
// Toggle the bookmark at this page: 1 added, 0 removed, -1 on failure.
int reader_store_bookmark_toggle(uint32_t book_id, uint32_t offset,
                                 uint16_t chapter);
// Remove exactly one bookmark. Returns 1 when removed, 0 when absent.
int reader_store_bookmark_remove(uint32_t book_id, uint32_t offset);

// A short label made from the book text AT the bookmark offset (not the chapter
// name). Returns bytes written, 0 when there is no text there, -1 on bad args.
int reader_store_bookmark_excerpt(const char *id, uint32_t offset,
                                  char *out, size_t out_size);

// Delete a book AND its persisted progress/bookmarks, so re-uploading the same
// filename starts clean. Deletable built-in samples are hidden in NVS instead
// of being removed from Flash; only the guide is protected (NOT_SUPPORTED).
esp_err_t reader_store_delete_book(const char *id);
