// reader_zdx.c — see reader_zdx.h. Pure C (stdio + uzlib); no ESP-IDF/LVGL,
// so the exact device decoder is compiled into the host tests as well.
#include "reader_zdx.h"

#include <string.h>

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Source abstraction: uploaded books read from a FILE, built-in books from a
// flash-backed memory image. Both seek to an absolute byte and read a block.
static int src_seek(reader_zdx_t *z, long pos)
{
    if (pos < 0) return -1;
    if (z->fp) return fseek(z->fp, pos, SEEK_SET) == 0 ? 0 : -1;
    if (z->mem) {
        if ((size_t)pos > z->mem_len) return -1;
        z->mem_pos = (size_t)pos;
        return 0;
    }
    return -1;
}

static size_t src_read(reader_zdx_t *z, void *buf, size_t len)
{
    if (z->fp) return fread(buf, 1, len, z->fp);
    if (z->mem) {
        if (z->mem_pos > z->mem_len) return 0;
        size_t avail = z->mem_len - z->mem_pos;
        if (len > avail) len = avail;
        memcpy(buf, z->mem + z->mem_pos, len);
        z->mem_pos += len;
        return len;
    }
    return 0;
}

static long src_size(reader_zdx_t *z)
{
    if (z->fp) {
        if (fseek(z->fp, 0, SEEK_END) != 0) return -1;
        return ftell(z->fp);
    }
    if (z->mem) return (long)z->mem_len;
    return -1;
}

int reader_zdx_probe(FILE *fp, uint32_t *original_size, uint32_t *text_chars)
{
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_SET) != 0) return -1;
    unsigned char h[READER_ZDX_HEADER_SIZE];
    if (fread(h, 1, sizeof(h), fp) != sizeof(h)) return -1;
    if (memcmp(h, READER_ZDX_MAGIC, 4) != 0) return -1;
    if (h[4] != READER_ZDX_VERSION || (h[5] & 0xFEu) != 0) return -1;
    uint32_t chunk = rd_u32(h + 8);
    uint32_t count = rd_u32(h + 12);
    if (chunk == 0 || chunk > READER_ZDX_CHUNK || count == 0) return -1;
    if (original_size) *original_size = rd_u32(h + 16);
    if (text_chars) *text_chars = rd_u32(h + 20);
    return 0;
}

// Shared header validation. Caller has zeroed z and set the source fields.
static int open_prepared(reader_zdx_t *z)
{
    z->cached_index = -1;

    if (src_seek(z, 0) != 0) return -1;
    unsigned char h[READER_ZDX_HEADER_SIZE];
    if (src_read(z, h, sizeof(h)) != sizeof(h)) return -1;
    if (memcmp(h, READER_ZDX_MAGIC, 4) != 0) return -1;
    if (h[4] != READER_ZDX_VERSION || (h[5] & 0xFEu) != 0) return -1;

    uint32_t chunk_size = rd_u32(h + 8);
    uint32_t chunk_count = rd_u32(h + 12);
    uint32_t original = rd_u32(h + 16);
    if (chunk_size == 0 || chunk_size > READER_ZDX_CHUNK) return -1;
    if (chunk_count == 0 || chunk_count > 131072u) return -1;

    // A valid image with N chunks of at most chunk_size original bytes must
    // report original <= N * chunk_size, and the table must fit in the source.
    uint64_t cap = (uint64_t)chunk_count * chunk_size;
    if ((uint64_t)original > cap) return -1;
    uint64_t table_end = (uint64_t)READER_ZDX_HEADER_SIZE +
                         (uint64_t)chunk_count * READER_ZDX_TABLE_ENTRY;
    if (table_end > 0x7FFFFFFFuLL) return -1;

    long size = src_size(z);
    if (size < 0 || (uint64_t)size < table_end) return -1;

    z->chunk_size = chunk_size;
    z->chunk_count = chunk_count;
    z->original_size = original;
    z->text_chars = rd_u32(h + 20);
    z->data_start = (uint32_t)table_end;
    z->file_size = size;
    return 0;
}

int reader_zdx_open(reader_zdx_t *z, FILE *fp)
{
    if (!z || !fp) return -1;
    memset(z, 0, sizeof(*z));
    z->fp = fp;
    return open_prepared(z);
}

int reader_zdx_open_mem(reader_zdx_t *z, const unsigned char *data, size_t len)
{
    if (!z || !data || len == 0) return -1;
    memset(z, 0, sizeof(*z));
    z->mem = data;
    z->mem_len = len;
    return open_prepared(z);
}

// Locate chunk `idx` in the table, then inflate it into out[]. The chunk table
// is re-read from the file each time (no RAM table), which is the only way to
// keep memory fixed on a device without PSRAM.
static int load_chunk(reader_zdx_t *z, uint32_t idx)
{
    if (idx >= z->chunk_count) return -1;
    if (z->cached_index == (int)idx) return 0;

    long entry = (long)READER_ZDX_HEADER_SIZE +
                 (long)idx * (long)READER_ZDX_TABLE_ENTRY;
    if (src_seek(z, entry) != 0) return -1;
    unsigned char e[READER_ZDX_TABLE_ENTRY];
    if (src_read(z, e, sizeof(e)) != sizeof(e)) return -1;
    uint32_t off = rd_u32(e);
    uint32_t len = rd_u32(e + 4);
    if ((uint64_t)off + len > (uint64_t)z->file_size) return -1;
    if (len == 0 || len > READER_ZDX_COMP_CAP) return -1;

    if (src_seek(z, (long)off) != 0) return -1;
    if (src_read(z, z->comp, len) != len) return -1;

    memset(z->dict, 0, sizeof(z->dict));
    uzlib_uncompress_init(&z->infl, z->dict, (unsigned)z->chunk_size);
    z->infl.source = z->comp;
    z->infl.source_limit = z->comp + len;
    z->infl.dest_start = z->out;
    z->infl.dest = z->out;
    z->infl.dest_limit = z->out + z->chunk_size;
    z->infl.checksum_type = TINF_CHKSUM_NONE;

    int r = uzlib_uncompress(&z->infl);
    if (r != TINF_OK && r != TINF_DONE) return -1;
    size_t got = (size_t)(z->infl.dest - z->out);
    if (got == 0 || got > z->chunk_size) return -1;

    // A full chunk must decompress to exactly chunk_size bytes; only the final
    // chunk may be shorter. This catches corrupted tables early.
    if (idx + 1 < z->chunk_count && got != z->chunk_size) return -1;
    uint64_t consumed = (uint64_t)idx * z->chunk_size + got;
    if (consumed > z->original_size) return -1;
    if (idx + 1 == z->chunk_count && consumed != z->original_size) return -1;

    z->cached_index = (int)idx;
    z->cached_len = (uint32_t)got;
    return 0;
}

size_t reader_zdx_read(reader_zdx_t *z, size_t offset, char *buf, size_t len)
{
    if (!z || (!z->fp && !z->mem) || !buf || len == 0) return 0;
    if (offset >= z->original_size) return 0;
    size_t remaining = z->original_size - offset;
    if (len > remaining) len = remaining;

    size_t copied = 0;
    while (copied < len) {
        size_t pos = offset + copied;
        uint32_t idx = (uint32_t)(pos / z->chunk_size);
        size_t intra = pos % z->chunk_size;
        if (load_chunk(z, idx) != 0) break;
        size_t avail = z->cached_len - intra;
        if (avail == 0) break;
        size_t take = len - copied;
        if (take > avail) take = avail;
        memcpy(buf + copied, z->out + intra, take);
        copied += take;
    }
    return copied;
}

int reader_zdx_verify(reader_zdx_t *z)
{
    if (!z || (!z->fp && !z->mem)) return -1;
    uint64_t total = 0;
    for (uint32_t i = 0; i < z->chunk_count; ++i) {
        if (load_chunk(z, i) != 0) return -1;
        total += z->cached_len;
    }
    return total == z->original_size ? 0 : -1;
}

void reader_zdx_close(reader_zdx_t *z)
{
    if (!z) return;
    z->fp = NULL;
    z->mem = NULL;
    z->cached_index = -1;
}

uint32_t reader_zdx_original_size(const reader_zdx_t *z)
{
    return z ? z->original_size : 0;
}

uint32_t reader_zdx_text_chars(const reader_zdx_t *z)
{
    return z ? z->text_chars : 0;
}
