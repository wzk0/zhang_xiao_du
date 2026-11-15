// Host tests for the pure progress blob codec.
// Build: cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_reader_progress.c
//        main/reader_progress.c -o /tmp/test_reader_progress
#include "reader_progress.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void test_crc(void)
{
    // Known CRC-32 of "123456789".
    CHECK(reader_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(reader_crc32("", 0) == 0u);
}

static void test_book_id(void)
{
    uint32_t a = reader_book_id("book-a.txt");
    uint32_t b = reader_book_id("book-b.txt");
    CHECK(a != b);
    CHECK(reader_book_id("book-a.txt") == a);  // stable
    CHECK(reader_book_id(NULL) != 0u);
}

static void test_roundtrip(void)
{
    reader_progress_blob_t blob;
    reader_progress_init(&blob);
    CHECK(blob.magic == READER_PROGRESS_MAGIC);
    CHECK(blob.version == READER_PROGRESS_VERSION);
    CHECK(blob.count == 0);

    reader_progress_entry_t e = {
        .book_id = reader_book_id("demo.txt"),
        .offset = 123456,
        .chapter = 7,
        .flags = 0,
    };
    reader_progress_set(&blob, &e);
    CHECK(blob.count == 1);

    uint8_t buf[READER_PROGRESS_BLOB_SIZE];
    int n = reader_progress_serialize(&blob, buf, sizeof(buf));
    CHECK(n == (int)(READER_PROGRESS_HEADER_SIZE + READER_PROGRESS_ENTRY_SIZE));

    reader_progress_blob_t restored;
    CHECK(reader_progress_parse(buf, (size_t)n, &restored) == 0);
    CHECK(restored.count == 1);
    CHECK(restored.entries[0].book_id == e.book_id);
    CHECK(restored.entries[0].offset == 123456);
    CHECK(restored.entries[0].chapter == 7);

    reader_progress_entry_t found;
    CHECK(reader_progress_find(&restored, e.book_id, &found) == 1);
    CHECK(found.offset == 123456);
    CHECK(reader_progress_find(&restored, 0xDEADBEEFu, &found) == 0);
}

static void test_corruption(void)
{
    reader_progress_blob_t blob;
    reader_progress_init(&blob);
    for (uint32_t i = 0; i < 3; ++i) {
        reader_progress_entry_t e = { .book_id = 100 + i, .offset = i * 10, .chapter = 0, .flags = 0 };
        reader_progress_set(&blob, &e);
    }
    uint8_t buf[READER_PROGRESS_BLOB_SIZE];
    int n = reader_progress_serialize(&blob, buf, sizeof(buf));
    CHECK(n > 0);

    // A single flipped byte must be rejected and leave a clean default record.
    uint8_t bad[READER_PROGRESS_BLOB_SIZE];
    memcpy(bad, buf, (size_t)n);
    bad[READER_PROGRESS_HEADER_SIZE + 2] ^= 0x40;
    reader_progress_blob_t out;
    CHECK(reader_progress_parse(bad, (size_t)n, &out) == -1);
    CHECK(out.count == 0);
    CHECK(out.magic == READER_PROGRESS_MAGIC);

    // Wrong magic.
    memcpy(bad, buf, (size_t)n);
    bad[0] ^= 0xFF;
    CHECK(reader_progress_parse(bad, (size_t)n, &out) == -1);

    // Truncated blob.
    CHECK(reader_progress_parse(buf, (size_t)n - 1, &out) == -1);
    CHECK(reader_progress_parse(buf, 4, &out) == -1);
    CHECK(reader_progress_parse(NULL, 0, &out) == -1);

    // Version mismatch.
    memcpy(bad, buf, (size_t)n);
    bad[4] = 99;
    CHECK(reader_progress_parse(bad, (size_t)n, &out) == -1);

    // Out-of-range count (CRC will also fail, which is fine).
    memcpy(bad, buf, (size_t)n);
    bad[8] = 0xFF;
    CHECK(reader_progress_parse(bad, (size_t)n, &out) == -1);
}

static void test_update_and_fifo(void)
{
    reader_progress_blob_t blob;
    reader_progress_init(&blob);

    reader_progress_entry_t e = { .book_id = 7, .offset = 10, .chapter = 0, .flags = 0 };
    reader_progress_set(&blob, &e);
    e.offset = 999;
    reader_progress_set(&blob, &e);  // update, not duplicate
    CHECK(blob.count == 1);
    CHECK(blob.entries[0].offset == 999);

    for (uint32_t i = 0; i < READER_PROGRESS_MAX_BOOKS + 5; ++i) {
        reader_progress_entry_t x = { .book_id = 1000 + i, .offset = i, .chapter = 0, .flags = 0 };
        reader_progress_set(&blob, &x);
    }
    CHECK(blob.count == READER_PROGRESS_MAX_BOOKS);
    // The newest entry must be present; the oldest ones were evicted.
    uint32_t newest = 1000 + READER_PROGRESS_MAX_BOOKS + 5 - 1;
    reader_progress_entry_t found;
    CHECK(reader_progress_find(&blob, newest, &found) == 1);
    CHECK(reader_progress_find(&blob, 1000, &found) == 0);

    uint8_t buf[READER_PROGRESS_BLOB_SIZE];
    int n = reader_progress_serialize(&blob, buf, sizeof(buf));
    CHECK(n == (int)READER_PROGRESS_BLOB_SIZE);
    reader_progress_blob_t restored;
    CHECK(reader_progress_parse(buf, (size_t)n, &restored) == 0);
    CHECK(restored.count == READER_PROGRESS_MAX_BOOKS);
}

static void test_bounds(void)
{
    reader_progress_blob_t blob;
    reader_progress_init(&blob);
    uint8_t small[8];
    CHECK(reader_progress_serialize(&blob, small, sizeof(small)) == -1);
    // Empty record needs only the header.
    uint8_t buf[READER_PROGRESS_BLOB_SIZE];
    CHECK(reader_progress_serialize(&blob, buf, READER_PROGRESS_HEADER_SIZE) ==
          (int)READER_PROGRESS_HEADER_SIZE);
    CHECK(reader_progress_serialize(&blob, buf, READER_PROGRESS_HEADER_SIZE - 1) == -1);
}

static void test_remove(void)
{
    reader_progress_blob_t blob;
    reader_progress_init(&blob);
    reader_progress_entry_t a = {
        .book_id = 1, .offset = 100, .chapter = 0, .flags = 0
    };
    reader_progress_entry_t b = {
        .book_id = 2, .offset = 200, .chapter = 1, .flags = 0
    };
    reader_progress_set(&blob, &a);
    reader_progress_set(&blob, &b);
    CHECK(blob.count == 2);

    // Removing a deleted book's record must not disturb the other entries and
    // must leave a blob that still parses with a matching CRC.
    CHECK(reader_progress_remove(&blob, 1) == 1);
    CHECK(blob.count == 1);
    CHECK(blob.entries[0].book_id == 2);
    uint8_t buf[READER_PROGRESS_BLOB_SIZE];
    int n = reader_progress_serialize(&blob, buf, sizeof(buf));
    CHECK(n > 0);
    reader_progress_blob_t parsed;
    CHECK(reader_progress_parse(buf, (size_t)n, &parsed) == 0);
    CHECK(parsed.count == 1);
    CHECK(parsed.entries[0].offset == 200);

    CHECK(reader_progress_remove(&blob, 999) == 0);
    CHECK(reader_progress_remove(NULL, 1) == 0);
}

int main(void)
{
    test_crc();
    test_book_id();
    test_roundtrip();
    test_corruption();
    test_update_and_fifo();
    test_bounds();
    test_remove();

    if (failures) {
        printf("reader_progress: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_progress: PASS\n");
    return 0;
}
