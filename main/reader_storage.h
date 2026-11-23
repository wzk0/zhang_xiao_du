// reader_storage.h — pure arithmetic for the storage-information page.
//
// The device has a small, fixed book partition. Rather than showing raw bytes
// only, the page answers a question a reader actually has: "how much more can
// I keep?". The reference works and the sentence template live here, free of
// ESP-IDF and LVGL, so the generated hint can be checked by host tests.
#pragma once

#include <stddef.h>

// The reader-facing name of an upload: the browser's file name without a
// trailing ".txt"/".zdx". This is what gets stored next to the book, because the
// file NAME on flash is an id (possibly truncated and hashed) and the reader
// must see what they uploaded.
void reader_storage_clean_title(const char *file_name, char *out, size_t out_size);

// Shelf title for an uploaded book. `file_name` is the on-flash name
// ("名字~3f2a1b9c.zdx"); the result drops the ".zdx" extension and the
// "~%08lx" hash. That hash exists only to keep two truncated ids unique — it is
// an implementation detail the reader must never see on the shelf.
void reader_storage_display_title(const char *file_name, char *out, size_t out_size);

// Saturated percentage: total == 0 and part > total are both handled without
// dividing by zero or reporting a nonsense >100 value.
int reader_storage_percent(size_t part, size_t total);

// Maximum temporary upload payload, with headroom for filesystem metadata.
size_t reader_storage_upload_budget(size_t free_bytes);

// Compose one Chinese sentence estimating how many more books/articles fit in
// `free_bytes`. The reference work is picked from a small table of well-known
// titles according to how much space is left, so the numbers stay meaningful.
// Returns the byte count written (0 only on invalid arguments), or -1 when the
// buffer is too small. Any non-NULL buffer with out_size > 0 is always left as
// a valid NUL-terminated string, including the too-small and invalid-argument
// paths, so callers can safely use it as an empty caption.
int reader_storage_hint(size_t free_bytes, char *out, size_t out_size);
