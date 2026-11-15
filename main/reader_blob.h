// reader_blob.h — shared little-endian helpers for the NVS blob codecs.
//
// reader_progress.c and reader_bookmark.c store the same 12-byte entry layout
// (u32 book_id, u32 offset, u16 chapter, u16 flags) behind a 16-byte header
// with a CRC32. Both used to carry their own private copies of these four
// static helpers; keeping them here avoids the duplication without changing a
// single byte of the persisted format, so records already in NVS stay readable.
// The short names are exactly the ones both codecs already call.
#pragma once

#include <stdint.h>

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
