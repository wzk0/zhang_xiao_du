// Host tests for the on-device zdx decoder, compiled against the SAME
// reader_zdx.c and uzlib sources the firmware uses. The fixtures are generated
// by tools/zdx_codec.py (Python zlib), so a green test proves the C decoder
// interoperates with the reference encoder and the browser's pako output.
#include "reader_zdx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void expect_file(const char *path, const char *expected_path)
{
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (!fp) return;

    // Header-only probe must report the same sizes without a full decoder.
    uint32_t probe_orig = 0, probe_chars = 0;
    CHECK(reader_zdx_probe(fp, &probe_orig, &probe_chars) == 0);

    reader_zdx_t z;
    CHECK(reader_zdx_open(&z, fp) == 0);
    CHECK(z.chunk_size == READER_ZDX_CHUNK);
    CHECK(z.original_size > 0);
    CHECK(probe_orig == reader_zdx_original_size(&z));
    CHECK(probe_chars == reader_zdx_text_chars(&z));
    CHECK(reader_zdx_verify(&z) == 0);

    FILE *ex = fopen(expected_path, "rb");
    CHECK(ex != NULL);
    if (!ex) { reader_zdx_close(&z); fclose(fp); return; }
    fseek(ex, 0, SEEK_END);
    long exn = ftell(ex);
    fseek(ex, 0, SEEK_SET);
    CHECK(exn == (long)z.original_size);

    // Read in a deliberately awkward stride to exercise cross-chunk reads.
    static char got[777];
    static char want[777];
    size_t pos = 0;
    while (pos < (size_t)exn) {
        size_t want_n = (exn - pos) < (long)sizeof(got) ? (size_t)(exn - pos) : sizeof(got);
        size_t n_ex = fread(want, 1, want_n, ex);
        size_t n_zx = reader_zdx_read(&z, pos, got, want_n);
        CHECK(n_ex == n_zx);
        if (n_ex == n_zx) CHECK(memcmp(got, want, n_ex) == 0);
        pos += n_zx;
    }
    CHECK(pos == (size_t)exn);

    // Random seeks must be independent of read order (bookmarks/chapters).
    char probe[64];
    CHECK(reader_zdx_read(&z, 0, probe, 8) == 8);
    size_t tail = z.original_size > 32 ? z.original_size - 32 : 0;
    CHECK(reader_zdx_read(&z, tail, probe, sizeof(probe)) == 32);
    CHECK(reader_zdx_read(&z, z.original_size, probe, 8) == 0);
    CHECK(reader_zdx_read(&z, 0, probe, 0) == 0);

    reader_zdx_close(&z);
    fclose(ex);
    fclose(fp);
}

// Header-level corruption: rejected immediately by reader_zdx_open().
static void expect_open_rejected(const char *path)
{
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (!fp) return;
    reader_zdx_t z;
    CHECK(reader_zdx_open(&z, fp) != 0);
    fclose(fp);
}

// Data-level corruption: the header is structurally valid, but inflating all
// chunks must fail in reader_zdx_verify() (this is the upload gate).
static void expect_verify_rejected(const char *path)
{
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (!fp) return;
    reader_zdx_t z;
    if (reader_zdx_open(&z, fp) == 0) {
        CHECK(reader_zdx_verify(&z) != 0);
        reader_zdx_close(&z);
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        printf("usage: test_reader_zdx GOOD.zdx GOOD.txt "
               "BAD_MAGIC BAD_VERSION BAD_ORIGSIZE TRUNCATED BAD_TABLE\n");
        return 2;
    }
    expect_file(argv[1], argv[2]);
    expect_open_rejected(argv[3]);   // bad magic
    expect_open_rejected(argv[4]);   // bad version
    expect_open_rejected(argv[5]);   // original size > chunk capacity
    expect_verify_rejected(argv[6]); // truncated data
    expect_verify_rejected(argv[7]); // chunk length out of range

    if (failures) {
        printf("reader_zdx: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_zdx: PASS\n");
    return 0;
}
