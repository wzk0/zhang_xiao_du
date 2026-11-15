// reader_bookmark.c — pure bookmark blob codec. See reader_bookmark.h.
#include "reader_bookmark.h"

#include "reader_progress.h"   // reader_crc32

#include "reader_blob.h"

#include <string.h>

// Byte-order helpers live in reader_blob.h, shared with the progress codec.

void reader_bookmark_init(reader_bookmark_blob_t *blob)
{
    if (!blob) return;
    memset(blob, 0, sizeof(*blob));
    blob->magic = READER_BOOKMARK_MAGIC;
    blob->version = READER_BOOKMARK_VERSION;
}

int reader_bookmark_serialize(const reader_bookmark_blob_t *blob,
                              uint8_t *out, size_t out_size)
{
    if (!blob || !out || blob->count > READER_BOOKMARK_MAX) return -1;
    size_t needed = READER_BOOKMARK_HEADER_SIZE +
                    (size_t)blob->count * READER_BOOKMARK_ENTRY_SIZE;
    if (out_size < needed) return -1;

    put_u32(out + 0, blob->magic);
    put_u32(out + 4, blob->version);
    put_u32(out + 8, blob->count);
    put_u32(out + 12, 0);  // CRC placeholder, filled below

    for (uint32_t i = 0; i < blob->count; ++i) {
        uint8_t *e = out + READER_BOOKMARK_HEADER_SIZE +
                     (size_t)i * READER_BOOKMARK_ENTRY_SIZE;
        put_u32(e + 0, blob->entries[i].book_id);
        put_u32(e + 4, blob->entries[i].offset);
        put_u16(e + 8, blob->entries[i].chapter);
        put_u16(e + 10, blob->entries[i].flags);
    }

    put_u32(out + 12, reader_crc32(out, needed));
    return (int)needed;
}

int reader_bookmark_parse(const uint8_t *buf, size_t len,
                          reader_bookmark_blob_t *out)
{
    if (!out) return -1;
    reader_bookmark_init(out);
    if (!buf || len < READER_BOOKMARK_HEADER_SIZE) return -1;

    uint32_t magic = get_u32(buf + 0);
    uint32_t version = get_u32(buf + 4);
    uint32_t count = get_u32(buf + 8);
    uint32_t stored_crc = get_u32(buf + 12);
    if (magic != READER_BOOKMARK_MAGIC || version != READER_BOOKMARK_VERSION ||
        count > READER_BOOKMARK_MAX) {
        return -1;
    }
    size_t needed = READER_BOOKMARK_HEADER_SIZE +
                    (size_t)count * READER_BOOKMARK_ENTRY_SIZE;
    if (len < needed) return -1;

    uint8_t scratch[READER_BOOKMARK_BLOB_SIZE];
    memcpy(scratch, buf, needed);
    put_u32(scratch + 12, 0);
    if (reader_crc32(scratch, needed) != stored_crc) return -1;

    out->magic = magic;
    out->version = version;
    out->count = count;
    out->crc32 = stored_crc;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = buf + READER_BOOKMARK_HEADER_SIZE +
                           (size_t)i * READER_BOOKMARK_ENTRY_SIZE;
        out->entries[i].book_id = get_u32(e + 0);
        out->entries[i].offset = get_u32(e + 4);
        out->entries[i].chapter = get_u16(e + 8);
        out->entries[i].flags = get_u16(e + 10);
    }
    return 0;
}

int reader_bookmark_find(const reader_bookmark_blob_t *blob, uint32_t book_id,
                         uint32_t offset)
{
    if (!blob || blob->count > READER_BOOKMARK_MAX) return -1;
    for (uint32_t i = 0; i < blob->count; ++i) {
        if (blob->entries[i].book_id == book_id &&
            blob->entries[i].offset == offset) {
            return (int)i;
        }
    }
    return -1;
}

int reader_bookmark_add(reader_bookmark_blob_t *blob,
                        const reader_bookmark_entry_t *entry)
{
    if (!blob || !entry) return -1;
    if (blob->count > READER_BOOKMARK_MAX) reader_bookmark_init(blob);
    if (reader_bookmark_find(blob, entry->book_id, entry->offset) >= 0) return 0;

    if (blob->count < READER_BOOKMARK_MAX) {
        blob->entries[blob->count++] = *entry;
        return 1;
    }
    // Full: drop the oldest so the pages just bookmarked survive.
    memmove(&blob->entries[0], &blob->entries[1],
            (READER_BOOKMARK_MAX - 1) * sizeof(blob->entries[0]));
    blob->entries[READER_BOOKMARK_MAX - 1] = *entry;
    return 1;
}

int reader_bookmark_remove(reader_bookmark_blob_t *blob, uint32_t book_id,
                           uint32_t offset)
{
    if (!blob) return 0;
    int idx = reader_bookmark_find(blob, book_id, offset);
    if (idx < 0) return 0;
    uint32_t tail = blob->count - (uint32_t)idx - 1;
    if (tail > 0) {
        memmove(&blob->entries[idx], &blob->entries[idx + 1],
                tail * sizeof(blob->entries[0]));
    }
    blob->count--;
    return 1;
}

int reader_bookmark_remove_book(reader_bookmark_blob_t *blob, uint32_t book_id)
{
    if (!blob || blob->count > READER_BOOKMARK_MAX) return 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < blob->count; ++i) {
        if (blob->entries[i].book_id == book_id) continue;
        if (kept != i) blob->entries[kept] = blob->entries[i];
        ++kept;
    }
    int removed = (int)(blob->count - kept);
    blob->count = kept;
    return removed;
}

int reader_bookmark_list(const reader_bookmark_blob_t *blob, uint32_t book_id,
                         reader_bookmark_entry_t *out, int max)
{
    if (!blob || !out || max <= 0) return 0;
    int n = 0;
    for (uint32_t i = 0; i < blob->count && n < max; ++i) {
        if (blob->entries[i].book_id != book_id) continue;
        // Insertion sort by page offset keeps the list in reading order.
        int j = n++;
        while (j > 0 && out[j - 1].offset > blob->entries[i].offset) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = blob->entries[i];
    }
    return n;
}
