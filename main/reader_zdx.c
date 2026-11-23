// reader_zdx.c — see reader_zdx.h. Pure C (stdio + uzlib); no ESP-IDF/LVGL,
// so the exact device decoder is compiled into the host tests as well.
#include "reader_zdx.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Deflate wants a window that is a power of two; a chunk is one independent
// stream whose window is exactly its own size.
#define READER_ZDX_MIN_CHUNK 1024u
#define NO_INDEX             0xFFFFFFFFu

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_power_of_two(uint32_t v)
{
    return v && (v & (v - 1)) == 0;
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
    if (chunk < READER_ZDX_MIN_CHUNK || chunk > READER_ZDX_CHUNK || !is_power_of_two(chunk)) {
        return -1;
    }
    if (count == 0) return -1;
    if (original_size) *original_size = rd_u32(h + 16);
    if (text_chars) *text_chars = rd_u32(h + 20);
    return 0;
}

// Shared header validation. Caller has zeroed z and set the source fields.
static int open_prepared(reader_zdx_t *z)
{
    z->cache_index = NO_INDEX;
    z->dec_index = NO_INDEX;

    if (src_seek(z, 0) != 0) return -1;
    unsigned char h[READER_ZDX_HEADER_SIZE];
    if (src_read(z, h, sizeof(h)) != sizeof(h)) return -1;
    if (memcmp(h, READER_ZDX_MAGIC, 4) != 0) return -1;
    if (h[4] != READER_ZDX_VERSION || (h[5] & 0xFEu) != 0) return -1;

    uint32_t chunk_size = rd_u32(h + 8);
    uint32_t chunk_count = rd_u32(h + 12);
    uint32_t original = rd_u32(h + 16);
    // The chunk size doubles as the deflate window, so it must be a power of two
    // that fits the fixed dictionary buffer.
    if (chunk_size < READER_ZDX_MIN_CHUNK || chunk_size > READER_ZDX_CHUNK) return -1;
    if (!is_power_of_two(chunk_size)) return -1;
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

// uzlib input callback: refill `comp` from the file and hand back the next
// byte. Streaming the compressed side keeps the decoder's RAM independent of
// the chunk size (a 16 KiB chunk of Chinese text compresses to ~8 KiB).
static int chunk_source_read(TINF_DATA *d)
{
    reader_zdx_t *z = (reader_zdx_t *)((char *)d - offsetof(reader_zdx_t, infl));
    if (z->comp_pos >= z->comp_len) return -1;
    size_t want = z->comp_len - z->comp_pos;
    if (want > READER_ZDX_COMP_CAP) want = READER_ZDX_COMP_CAP;
    if (src_seek(z, (long)(z->comp_off + z->comp_pos)) != 0) return -1;
    size_t got = src_read(z, z->comp, want);
    if (got == 0) return -1;
    z->comp_pos += (uint32_t)got;
    d->source = z->comp;
    d->source_limit = z->comp + got;
    return *d->source++;
}

// Point the inflater at chunk `idx` (no output yet). Idempotent while that
// chunk stays selected, so a sequence of reads inside one chunk decodes once.
static int chunk_begin(reader_zdx_t *z, uint32_t idx)
{
    if (z->dec_index == idx) return 0;
    if (idx >= z->chunk_count) return -1;

    long entry = (long)READER_ZDX_HEADER_SIZE +
                 (long)idx * (long)READER_ZDX_TABLE_ENTRY;
    if (src_seek(z, entry) != 0) return -1;
    unsigned char e[READER_ZDX_TABLE_ENTRY];
    if (src_read(z, e, sizeof(e)) != sizeof(e)) return -1;
    uint32_t off = rd_u32(e);
    uint32_t len = rd_u32(e + 4);
    if ((uint64_t)off + len > (uint64_t)z->file_size) return -1;
    if (len == 0) return -1;

    z->comp_off = off;
    z->comp_len = len;
    z->comp_pos = 0;
    // Anything decoded so far described a different stream, and out[] is about
    // to be overwritten: drop the hold-back window with the old state.
    z->cache_index = NO_INDEX;
    z->cache_len = 0;
    z->dec_index = idx;
    z->dec_produced = 0;
    z->dec_done = 0;

    memset(z->dict, 0, z->chunk_size);
    uzlib_uncompress_init(&z->infl, z->dict, (unsigned)z->chunk_size);
    z->infl.source = NULL;
    z->infl.source_limit = NULL;
    z->infl.source_read_cb = chunk_source_read;
    z->infl.dest = z->out;
    z->infl.dest_start = z->out;
    z->infl.dest_limit = z->out;
    z->infl.checksum_type = TINF_CHKSUM_NONE;
    return 0;
}

// Inflate at most `max` original bytes into out[]. Returns the count produced
// (0 only when the chunk's stream has ended) or -1 on a decode error. A full
// chunk must produce exactly chunk_size bytes; only the last one may be short.
static int decode_into(reader_zdx_t *z, uint32_t max)
{
    if (max == 0) return 0;
    if (max > READER_ZDX_OUT_CAP) max = READER_ZDX_OUT_CAP;
    if (z->dec_done) return 0;

    z->infl.dest = z->out;
    z->infl.dest_start = z->out;
    z->infl.dest_limit = z->out + max;
    int r = uzlib_uncompress(&z->infl);
    size_t got = (size_t)(z->infl.dest - z->out);
    if (r == TINF_DONE) {
        z->dec_done = 1;
        // Structural check: everything except the final chunk is exactly
        // chunk_size original bytes long.
        uint32_t expect = z->dec_index + 1 < z->chunk_count ? z->chunk_size
                          : z->original_size - (uint32_t)z->dec_index * z->chunk_size;
        if (z->dec_produced + got != expect) return -1;
    } else if (r != TINF_OK) {
        return -1;
    }
    z->dec_produced += (uint32_t)got;
    if (!z->dec_done && got < max) return -1;  // stream stalled without filling
    return (int)got;
}

// Fill the hold-back window with bytes starting at `intra` of chunk `idx`.
static int fill_cache(reader_zdx_t *z, uint32_t idx, uint32_t intra)
{
    // Re-inflating always starts at the chunk's first byte, so a request behind
    // the current decode position has to restart the stream. Reads that move
    // forward keep going from where the inflater is.
    if (z->dec_index != idx || intra < z->dec_produced) {
        z->dec_index = NO_INDEX;
        if (chunk_begin(z, idx) != 0) return -1;
    }

    while (z->dec_produced < intra) {
        uint32_t step = intra - z->dec_produced;
        int got = decode_into(z, step);
        if (got < 0) return -1;
        if (got == 0) return -1;  // chunk ended before the read offset
    }

    int got = decode_into(z, READER_ZDX_OUT_CAP);
    if (got < 0) return -1;
    z->cache_index = idx;
    z->cache_start = intra;
    z->cache_len = (uint32_t)got;
    return got > 0 ? 0 : -1;
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
        uint32_t intra = (uint32_t)(pos % z->chunk_size);

        // The window serves consecutive reads; anything outside it re-inflates
        // the chunk from its start (bounded by the chunk size, not the file).
        bool covered = z->cache_index == idx &&
                       intra >= z->cache_start &&
                       intra < z->cache_start + z->cache_len;
        if (!covered && fill_cache(z, idx, intra) != 0) break;

        uint32_t avail = z->cache_start + z->cache_len - intra;
        size_t take = len - copied;
        if (take > avail) take = avail;
        if (take == 0) break;
        memcpy(buf + copied, z->out + (intra - z->cache_start), take);
        copied += take;
    }
    return copied;
}

int reader_zdx_verify(reader_zdx_t *z)
{
    if (!z || (!z->fp && !z->mem)) return -1;
    uint64_t total = 0;
    for (uint32_t i = 0; i < z->chunk_count; ++i) {
        if (chunk_begin(z, i) != 0) return -1;
        while (!z->dec_done) {
            if (decode_into(z, READER_ZDX_OUT_CAP) < 0) return -1;
        }
        total += z->dec_produced;
        if (total > z->original_size) return -1;
    }
    return total == z->original_size ? 0 : -1;
}

void reader_zdx_close(reader_zdx_t *z)
{
    if (!z) return;
    z->fp = NULL;
    z->mem = NULL;
    z->cache_index = NO_INDEX;
    z->dec_index = NO_INDEX;
}

uint32_t reader_zdx_original_size(const reader_zdx_t *z)
{
    return z ? z->original_size : 0;
}

uint32_t reader_zdx_text_chars(const reader_zdx_t *z)
{
    return z ? z->text_chars : 0;
}