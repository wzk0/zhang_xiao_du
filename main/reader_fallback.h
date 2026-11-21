// Shared bitmap strike for Chinese characters outside the primary fonts.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Return a zero-based glyph index, or -1 if the code point is absent.
int reader_fallback_lookup(uint32_t cp);

// LVGL glyph offset, aligned with the primary sizes' 16 px strike baseline.
int reader_fallback_offset_y(unsigned size);

// Render a square glyph into an A8 buffer owned by the caller.
bool reader_fallback_draw(unsigned index, unsigned size, uint8_t *out, size_t stride);
