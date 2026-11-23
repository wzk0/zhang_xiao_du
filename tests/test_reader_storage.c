// Host tests for the storage page's pure arithmetic and generated hint.
// Build: cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_reader_storage.c
//        main/reader_storage.c -o /tmp/test_reader_storage
#include "reader_storage.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void test_percent(void)
{
    CHECK(reader_storage_percent(0, 0) == 0);
    CHECK(reader_storage_percent(100, 0) == 0);
    CHECK(reader_storage_percent(0, 4096) == 0);
    CHECK(reader_storage_percent(2048, 4096) == 50);
    CHECK(reader_storage_percent(4096, 4096) == 100);
    CHECK(reader_storage_percent(9999, 4096) == 100);  // saturated
    // A 4 MiB partition times 100 must not overflow a 32-bit size_t.
    CHECK(reader_storage_percent(1024u * 1024u, 4u * 1024u * 1024u) == 25);
}

static void test_upload_budget(void)
{
    const size_t kib = 1024u;
    CHECK(reader_storage_upload_budget(0) == 0);
    CHECK(reader_storage_upload_budget(8 * kib) == 0);
    CHECK(reader_storage_upload_budget(64 * kib) == 55 * kib);
    CHECK(reader_storage_upload_budget(2 * 1024 * kib) == 2008 * kib);
    CHECK(reader_storage_upload_budget(2 * 1024 * kib) > 512 * kib);
    CHECK(reader_storage_upload_budget((size_t)-1) < (size_t)-1);
}

static void test_hint_bounds(void)
{
    char out[128];
    CHECK(reader_storage_hint(0, NULL, 0) == -1);
    char tiny[8];
    // Seed with non-zero bytes. The assertion below must prove that the callee
    // terminates the buffer, not that a clean stack happens to start with 0.
    memset(tiny, 'X', sizeof(tiny));
    CHECK(reader_storage_hint(1000, tiny, sizeof(tiny)) == -1);
    CHECK(tiny[0] == '\0');
    CHECK(reader_storage_hint(0, out, sizeof(out)) > 0);
    CHECK(strstr(out, "空间已满") != NULL);
    CHECK(strlen(out) < sizeof(out));
}

static void test_hint_scales(void)
{
    char out[128];
    const size_t mib = 1024u * 1024u;

    // A nearly empty 4 MiB partition uses the long-work yardstick and reports
    // both a book count and an article count in ONE generated sentence.
    CHECK(reader_storage_hint(4 * mib - 4096, out, sizeof(out)) > 0);
    CHECK(strstr(out, "约可再存") != NULL);
    CHECK(strstr(out, "本《红楼梦》") != NULL);
    CHECK(strstr(out, "篇《岳阳楼记》") != NULL);
    CHECK(strlen(out) < sizeof(out));

    // A smaller remainder switches to the short-poem reference set.
    CHECK(reader_storage_hint(100u * 1024u, out, sizeof(out)) > 0);
    CHECK(strstr(out, "本《唐诗三百首》") != NULL);
    CHECK(strstr(out, "篇《静夜思》") != NULL);

    // Large counts are compacted with 万 instead of a six-digit number.
    CHECK(reader_storage_hint(700u * 1024u, out, sizeof(out)) > 0);
    CHECK(strstr(out, "万篇") != NULL);

    // Too little room for even one short poem: a plain-language warning.
    CHECK(reader_storage_hint(20, out, sizeof(out)) > 0);
    CHECK(strstr(out, "剩余空间不多") != NULL);
}

static void test_hint_is_complete_utf8(void)
{
    char out[128];
    CHECK(reader_storage_hint(3u * 1024u * 1024u, out, sizeof(out)) > 0);
    size_t len = strlen(out), pos = 0;
    while (pos < len) {
        unsigned char c = (unsigned char)out[pos];
        size_t need = c < 0x80 ? 1 :
                      (c & 0xE0) == 0xC0 ? 2 :
                      (c & 0xF0) == 0xE0 ? 3 :
                      (c & 0xF8) == 0xF0 ? 4 : 1;
        CHECK(pos + need <= len);
        for (size_t i = 1; i < need && pos + i < len; ++i) {
            CHECK(((unsigned char)out[pos + i] & 0xC0) == 0x80);
        }
        pos += need;
    }
}

// The shelf must show a book's NAME, never the "~%08lx" uniqueness hash that
// sanitize_name() appends to a shortened file name.
static void test_display_title(void)
{
    char out[64];

    reader_storage_display_title("短中篇幼幼~42b8c6db.zdx", out, sizeof(out));
    CHECK(strcmp(out, "短中篇幼幼") == 0);

    reader_storage_display_title("三体.zdx", out, sizeof(out));
    CHECK(strcmp(out, "三体") == 0);

    // Uppercase hex is not what the generator emits; keep it visible.
    reader_storage_display_title("book~DEADBEEF.zdx", out, sizeof(out));
    CHECK(strcmp(out, "book~DEADBEEF") == 0);

    // Not exactly eight digits: not a hash, keep the name.
    reader_storage_display_title("第一版~2024.zdx", out, sizeof(out));
    CHECK(strcmp(out, "第一版~2024") == 0);

    // A name that is only the hash still needs a label, not an empty row.
    reader_storage_display_title("~1a2b3c4d.zdx", out, sizeof(out));
    CHECK(out[0] != '\0');

    // Truncation is bounded and NUL-terminated.
    reader_storage_display_title("一二三四五六七八九十一二三四五六七八九十.zdx", out, 16);
    CHECK(strlen(out) == 15);
    CHECK(out[15] == '\0');

    reader_storage_display_title("", out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    reader_storage_display_title(NULL, out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
}

int main(void)
{
    test_percent();
    test_upload_budget();
    test_hint_bounds();
    test_hint_scales();
    test_display_title();
    test_hint_is_complete_utf8();

    if (failures) {
        printf("reader_storage: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_storage: PASS\n");
    return 0;
}
