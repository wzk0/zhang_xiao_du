// reader_fonts.c — see reader_fonts.h.
#include "reader_fonts.h"

#include "reader_layout.h"

#include "../assets/fonts/reader_charset.h"

LV_FONT_DECLARE(lv_font_reader_12);
LV_FONT_DECLARE(lv_font_reader_13);
LV_FONT_DECLARE(lv_font_reader_14);
LV_FONT_DECLARE(lv_font_reader_15);
LV_FONT_DECLARE(lv_font_reader_16);
LV_FONT_DECLARE(lv_font_reader_17);
LV_FONT_DECLARE(lv_font_reader_18);
LV_FONT_DECLARE(lv_font_reader_19);
LV_FONT_DECLARE(lv_font_reader_20);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);

#define READER_FONT_COUNT (READER_FONT_MAX_SIZE - READER_FONT_MIN_SIZE + 1)

// Writable shallow copies. The referenced bitmaps stay const in Flash; only the
// descriptor (and its fallback pointer) is copied, with application lifetime.
static lv_font_t s_fonts[READER_FONT_COUNT];

static const lv_font_t *const k_flash_fonts[READER_FONT_COUNT] = {
    &lv_font_reader_12, &lv_font_reader_13, &lv_font_reader_14,
    &lv_font_reader_15, &lv_font_reader_16, &lv_font_reader_17,
    &lv_font_reader_18, &lv_font_reader_19, &lv_font_reader_20,
};

void reader_fonts_init(void)
{
    for (int i = 0; i < READER_FONT_COUNT; ++i) {
        s_fonts[i] = *k_flash_fonts[i];
        // Symbols outside the subset fall back to the matching LVGL built-in
        // size rather than to tofu; nothing in the inventory should need it.
        s_fonts[i].fallback =
            s_fonts[i].line_height >= 22 ? &lv_font_montserrat_20
                                         : &lv_font_montserrat_14;
    }
}

const lv_font_t *reader_font(int size)
{
    return &s_fonts[reader_layout_font_index(size)];
}

int reader_font_supported(uint32_t cp)
{
    if (cp < 0x80) return 1;  // printable ASCII is always in the inventory
    return reader_charset_has(cp);
}

int reader_font_supported_cb(uint32_t cp, void *ctx)
{
    (void)ctx;
    return reader_font_supported(cp);
}

