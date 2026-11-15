// reader_progress.c — pure progress blob codec. See reader_progress.h.
#include "reader_progress.h"

#include "reader_blob.h"

#include <string.h>

uint32_t reader_book_id(const char *name)
{
    // FNV-1a 32-bit; deterministic across host and target.
    uint32_t h = 2166136261u;
    if (!name) return h;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

uint32_t reader_crc32(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Byte-order helpers live in reader_blob.h, shared with the bookmark codec.

void reader_progress_init(reader_progress_blob_t *blob)
{
    if (!blob) return;
    memset(blob, 0, sizeof(*blob));
    blob->magic = READER_PROGRESS_MAGIC;
    blob->version = READER_PROGRESS_VERSION;
}

int reader_progress_serialize(const reader_progress_blob_t *blob,
                              uint8_t *out, size_t out_size)
{
    if (!blob || !out || blob->count > READER_PROGRESS_MAX_BOOKS) return -1;
    size_t needed = READER_PROGRESS_HEADER_SIZE +
                    (size_t)blob->count * READER_PROGRESS_ENTRY_SIZE;
    if (out_size < needed) return -1;

    put_u32(out + 0, blob->magic);
    put_u32(out + 4, blob->version);
    put_u32(out + 8, blob->count);
    put_u32(out + 12, 0);  // CRC placeholder, filled below

    for (uint32_t i = 0; i < blob->count; ++i) {
        uint8_t *e = out + READER_PROGRESS_HEADER_SIZE +
                     (size_t)i * READER_PROGRESS_ENTRY_SIZE;
        put_u32(e + 0, blob->entries[i].book_id);
        put_u32(e + 4, blob->entries[i].offset);
        put_u16(e + 8, blob->entries[i].chapter);
        put_u16(e + 10, blob->entries[i].flags);
    }

    // CRC covers the header (with the CRC field still zero) and all entries.
    uint32_t crc = reader_crc32(out, needed);
    put_u32(out + 12, crc);
    return (int)needed;
}

int reader_progress_parse(const uint8_t *buf, size_t len,
                          reader_progress_blob_t *out)
{
    if (!out) return -1;
    reader_progress_init(out);
    if (!buf || len < READER_PROGRESS_HEADER_SIZE) return -1;

    uint32_t magic = get_u32(buf + 0);
    uint32_t version = get_u32(buf + 4);
    uint32_t count = get_u32(buf + 8);
    uint32_t stored_crc = get_u32(buf + 12);
    if (magic != READER_PROGRESS_MAGIC || version != READER_PROGRESS_VERSION ||
        count > READER_PROGRESS_MAX_BOOKS) {
        return -1;
    }
    size_t needed = READER_PROGRESS_HEADER_SIZE +
                    (size_t)count * READER_PROGRESS_ENTRY_SIZE;
    if (len < needed) return -1;

    // Recompute with the CRC field zeroed.
    uint8_t scratch[READER_PROGRESS_BLOB_SIZE];
    memcpy(scratch, buf, needed);
    put_u32(scratch + 12, 0);
    if (reader_crc32(scratch, needed) != stored_crc) return -1;

    out->magic = magic;
    out->version = version;
    out->count = count;
    out->crc32 = stored_crc;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = buf + READER_PROGRESS_HEADER_SIZE +
                           (size_t)i * READER_PROGRESS_ENTRY_SIZE;
        out->entries[i].book_id = get_u32(e + 0);
        out->entries[i].offset = get_u32(e + 4);
        out->entries[i].chapter = get_u16(e + 8);
        out->entries[i].flags = get_u16(e + 10);
    }
    return 0;
}

int reader_progress_find(const reader_progress_blob_t *blob, uint32_t book_id,
                         reader_progress_entry_t *out)
{
    if (!blob || blob->count > READER_PROGRESS_MAX_BOOKS) return 0;
    for (uint32_t i = 0; i < blob->count; ++i) {
        if (blob->entries[i].book_id == book_id) {
            if (out) *out = blob->entries[i];
            return 1;
        }
    }
    return 0;
}

void reader_progress_set(reader_progress_blob_t *blob,
                         const reader_progress_entry_t *entry)
{
    if (!blob || !entry) return;
    if (blob->count > READER_PROGRESS_MAX_BOOKS) {
        reader_progress_init(blob);
    }
    for (uint32_t i = 0; i < blob->count; ++i) {
        if (blob->entries[i].book_id == entry->book_id) {
            blob->entries[i] = *entry;
            return;
        }
    }
    if (blob->count < READER_PROGRESS_MAX_BOOKS) {
        blob->entries[blob->count++] = *entry;
        return;
    }
    // Full: drop the oldest (index 0), shift, append.
    memmove(&blob->entries[0], &blob->entries[1],
            (READER_PROGRESS_MAX_BOOKS - 1) * sizeof(blob->entries[0]));
    blob->entries[READER_PROGRESS_MAX_BOOKS - 1] = *entry;
}

int reader_progress_remove(reader_progress_blob_t *blob, uint32_t book_id)
{
    if (!blob || blob->count > READER_PROGRESS_MAX_BOOKS) return 0;
    for (uint32_t i = 0; i < blob->count; ++i) {
        if (blob->entries[i].book_id != book_id) continue;
        uint32_t tail = blob->count - i - 1;
        if (tail > 0) {
            memmove(&blob->entries[i], &blob->entries[i + 1],
                    tail * sizeof(blob->entries[0]));
        }
        blob->count--;
        return 1;
    }
    return 0;
}
