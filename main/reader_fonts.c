// reader_fonts.c — see reader_fonts.h.
#include "reader_fonts.h"

#include "reader_fallback.h"
#include "reader_layout.h"

#include "../assets/fonts/reader_charset.h"

LV_FONT_DECLARE(lv_font_reader_12);
LV_FONT_DECLARE(lv_font_reader_13);
LV_FONT_DECLARE(lv_font_reader_14);
LV_FONT_DECLARE(lv_font_reader_15);
LV_FONT_DECLARE(lv_font_reader_16);
LV_FONT_DECLARE(lv_font_reader_18);
LV_FONT_DECLARE(lv_font_reader_20);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);

#define READER_FONT_COUNT READER_FONT_SIZE_COUNT

// Writable shallow copies. The referenced bitmaps stay const in Flash; only the
// descriptor (and its fallback pointer) is copied, with application lifetime.
static lv_font_t s_fonts[READER_FONT_COUNT];
static lv_font_t s_fallback_fonts[READER_FONT_COUNT];

// One entry per row of g_reader_font_metrics, in the same order.
static const lv_font_t *const k_flash_fonts[READER_FONT_COUNT] = {
    &lv_font_reader_12, &lv_font_reader_13, &lv_font_reader_14,
    &lv_font_reader_15, &lv_font_reader_16, &lv_font_reader_18,
    &lv_font_reader_20,
};

static bool fallback_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                  uint32_t letter, uint32_t next)
{
    (void)next;
    int index = reader_fallback_lookup(letter);
    if (index < 0) return false;
    const reader_font_metrics_t *metrics = font->dsc;
    uint8_t size = metrics->size;
    *out = (lv_font_glyph_dsc_t){
        .adv_w = size,
        .box_w = size,
        .box_h = size,
        .ofs_y = (int16_t)reader_fallback_offset_y(size),
        .format = LV_FONT_GLYPH_FORMAT_A8,
        .gid.index = (uint32_t)index,
    };
    return true;
}

static const void *fallback_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buf)
{
    // The LVGL software renderer owns the scratch buffer for this glyph.
    if (!buf || !reader_fallback_draw(glyph->gid.index, glyph->box_w, buf->data,
                                       buf->header.stride)) return NULL;
    return buf;
}

void reader_fonts_init(void)
{
    for (int i = 0; i < READER_FONT_COUNT; ++i) {
        s_fonts[i] = *k_flash_fonts[i];
        s_fallback_fonts[i] = (lv_font_t){
            .get_glyph_dsc = fallback_descriptor,
            .get_glyph_bitmap = fallback_bitmap,
            .line_height = s_fonts[i].line_height,
            .base_line = s_fonts[i].base_line,
            .dsc = &g_reader_font_metrics[i],
            .fallback = s_fonts[i].line_height >= 22 ? &lv_font_montserrat_20
                                                     : &lv_font_montserrat_14,
        };
        s_fonts[i].fallback = &s_fallback_fonts[i];
    }
}

const lv_font_t *reader_font(int size)
{
    return &s_fonts[reader_layout_font_index(size)];
}

int reader_font_supported(uint32_t cp)
{
    if (cp < 0x80) return 1;  // printable ASCII is always in the inventory
    return reader_charset_has(cp) || reader_fallback_lookup(cp) >= 0;
}

int reader_font_supported_cb(uint32_t cp, void *ctx)
{
    (void)ctx;
    return reader_font_supported(cp);
}

