// reader_fonts.h — application font selection and glyph coverage.
//
// The generated fonts are const, Flash-resident LVGL bitmaps. Widgets must use
// the application-owned descriptors returned by reader_font() so a fallback
// font is available for symbols outside the subset. Coverage queries that run
// outside the LVGL task (the HTTP upload worker) use the plain C code-point
// table and must never call into LVGL.
#pragma once

#include "lvgl.h"

#include <stdint.h>

// Call once after bsp_lvgl_init() and before any widget is created, inside the
// LVGL task or while holding bsp_lvgl_lock().
void reader_fonts_init(void);

// Returns the application font for a body size in
// [READER_FONT_MIN_SIZE, READER_FONT_MAX_SIZE]; out-of-range values are
// clamped. Always non-NULL after init.
const lv_font_t *reader_font(int size);

// Pure coverage query (no LVGL access). ASCII is always supported.
int reader_font_supported(uint32_t cp);

// Callback adapter for reader_scan_charset().
int reader_font_supported_cb(uint32_t cp, void *ctx);

