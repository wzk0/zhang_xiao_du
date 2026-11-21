// reader_storage.h — pure arithmetic for the storage-information page.
//
// The device has a small, fixed book partition. Rather than showing raw bytes
// only, the page answers a question a reader actually has: "how much more can
// I keep?". The reference works and the sentence template live here, free of
// ESP-IDF and LVGL, so the generated hint can be checked by host tests.
#pragma once

#include <stddef.h>

// Saturated percentage: total == 0 and part > total are both handled without
// dividing by zero or reporting a nonsense >100 value.
int reader_storage_percent(size_t part, size_t total);

// Maximum temporary upload payload, with headroom for SPIFFS metadata.
size_t reader_storage_upload_budget(size_t free_bytes);

// Compose one Chinese sentence estimating how many more books/articles fit in
// `free_bytes`. The reference work is picked from a small table of well-known
// titles according to how much space is left, so the numbers stay meaningful.
// Returns the byte count written (0 only on invalid arguments), or -1 when the
// buffer is too small. Any non-NULL buffer with out_size > 0 is always left as
// a valid NUL-terminated string, including the too-small and invalid-argument
// paths, so callers can safely use it as an empty caption.
int reader_storage_hint(size_t free_bytes, char *out, size_t out_size);
