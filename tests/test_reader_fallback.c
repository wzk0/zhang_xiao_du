#include "reader_fallback.h"
#include "reader_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint32_t samples[] = {0x4E0C, 0x59A4, 0x6DFC, 0x946B, 0x9F44};
    uint8_t pixels[READER_FONT_MAX_SIZE][READER_FONT_MAX_SIZE + 4];

    assert(reader_fallback_lookup(0x4E00) == -1);  // Primary font only.
    assert(reader_fallback_lookup(0x9F98) == -1);  // Outside GB2312.
    assert(reader_fallback_lookup(0x1F600) == -1);
    assert(reader_fallback_offset_y(16) == -3);

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        int glyph = reader_fallback_lookup(samples[i]);
        assert(glyph >= 0);
        for (unsigned size = READER_FONT_MIN_SIZE; size <= READER_FONT_MAX_SIZE; ++size) {
            memset(pixels, 0x55, sizeof(pixels));
            assert(reader_fallback_draw((unsigned)glyph, size, &pixels[0][0], sizeof(pixels[0])));
            unsigned ink = 0;
            for (unsigned y = 0; y < READER_FONT_MAX_SIZE; ++y) {
                for (size_t x = 0; x < sizeof(pixels[0]); ++x) {
                    if (y < size && x < size) {
                        assert(pixels[y][x] == 0 || pixels[y][x] == 255);
                        ink += pixels[y][x] == 255;
                    } else {
                        assert(pixels[y][x] == 0x55);
                    }
                }
            }
            assert(ink > 0 && ink < size * size);
        }
    }

    assert(!reader_fallback_draw(0, 16, &pixels[0][0], 15));
    assert(!reader_fallback_draw(3008, 16, &pixels[0][0], sizeof(pixels[0])));
    puts("reader_fallback: PASS");
    return 0;
}
