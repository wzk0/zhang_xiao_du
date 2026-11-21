#include "reader_fallback.h"

#include "reader_layout.h"
#include "../assets/fonts/reader_fallback_data.h"

#define STRIKE_SIZE 16u
#define GLYPH_COUNT (sizeof(fallback_codepoints) / sizeof(fallback_codepoints[0]))

_Static_assert(GLYPH_COUNT == sizeof(fallback_rows) / sizeof(fallback_rows[0]),
               "each code point must have a bitmap");

int reader_fallback_lookup(uint32_t cp)
{
    size_t lo = 0, hi = GLYPH_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (fallback_codepoints[mid] == cp) return (int)mid;
        if (fallback_codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

int reader_fallback_offset_y(unsigned size)
{
    unsigned baseline = (READER_FALLBACK_BASELINE * size + STRIKE_SIZE / 2) / STRIKE_SIZE;
    return (int)baseline - (int)size;
}

static void extend_rows(const uint32_t *src, uint32_t *dst, unsigned size)
{
    unsigned used = 0, previous = 0;
    dst[used++] = src[0];
    for (unsigned r = 1; r < STRIKE_SIZE; ++r) {
        unsigned position = (r * (size - 1) + 7) / (STRIKE_SIZE - 1);
        if (position - previous == 2) {
            uint32_t upper = src[r - 1], lower = src[r];
            // Keep connections across the inserted row. Where a straight
            // connection exists, avoid adding ink beside it at the joint.
            uint32_t diagonal = lower & ~upper;
            dst[used++] = upper & (lower | diagonal << 1 | diagonal >> 1);
        }
        dst[used++] = src[r];
        previous = position;
    }
}

static void draw_large(const uint16_t *glyph, unsigned size, uint8_t *out, size_t stride)
{
    uint32_t source[STRIKE_SIZE], rows[READER_FONT_MAX_SIZE];
    uint32_t cols[STRIKE_SIZE] = {0}, extended[READER_FONT_MAX_SIZE];
    for (unsigned r = 0; r < STRIKE_SIZE; ++r) source[r] = glyph[r];
    extend_rows(source, rows, size);
    for (unsigned c = 0; c < STRIKE_SIZE; ++c) {
        for (unsigned r = 0; r < size; ++r) {
            cols[c] |= ((rows[r] >> c) & 1u) << r;
        }
    }
    extend_rows(cols, extended, size);
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            out[y * stride + x] = (extended[x] & (1u << y)) ? 255 : 0;
        }
    }
}

static unsigned overlap(unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned end = b < d ? b : d;
    unsigned start = a > c ? a : c;
    return end > start ? end - start : 0;
}

static void draw_small(const uint16_t *glyph, unsigned size, uint8_t *out, size_t stride)
{
    // Area sampling keeps thin strokes that nearest-neighbour reduction can
    // discard. The result remains binary, like the primary fonts.
    for (unsigned y = 0; y < size; ++y) {
        unsigned top = y * STRIKE_SIZE, bottom = top + STRIKE_SIZE;
        for (unsigned x = 0; x < size; ++x) {
            unsigned left = x * STRIKE_SIZE, right = left + STRIKE_SIZE;
            unsigned ink = 0;
            for (unsigned r = top / size; r * size < bottom; ++r) {
                unsigned height = overlap(top, bottom, r * size, (r + 1) * size);
                for (unsigned c = left / size; c * size < right; ++c) {
                    if (glyph[r] & (1u << c)) {
                        ink += height * overlap(left, right, c * size, (c + 1) * size);
                    }
                }
            }
            out[y * stride + x] = ink >= STRIKE_SIZE * STRIKE_SIZE * 3 / 8 ? 255 : 0;
        }
    }
}

bool reader_fallback_draw(unsigned index, unsigned size, uint8_t *out, size_t stride)
{
    if (!out || index >= GLYPH_COUNT || size < READER_FONT_MIN_SIZE ||
        size > READER_FONT_MAX_SIZE || stride < size) return false;

    const uint16_t *glyph = fallback_rows[index];
    if (size > STRIKE_SIZE) {
        draw_large(glyph, size, out, stride);
    } else if (size < STRIKE_SIZE) {
        draw_small(glyph, size, out, stride);
    } else {
        for (unsigned y = 0; y < size; ++y) {
            for (unsigned x = 0; x < size; ++x) {
                out[y * stride + x] = (glyph[y] & (1u << x)) ? 255 : 0;
            }
        }
    }
    return true;
}
