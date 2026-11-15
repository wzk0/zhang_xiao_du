// Host tests for the pure bookmark blob codec.
// Build: cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_reader_bookmark.c
//        main/reader_bookmark.c main/reader_progress.c -o /tmp/test_reader_bookmark
#include "reader_bookmark.h"
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

static reader_bookmark_entry_t make(uint32_t book, uint32_t offset, uint16_t chapter)
{
    reader_bookmark_entry_t e = {
        .book_id = book,
        .offset = offset,
        .chapter = chapter,
        .flags = 0,
    };
    return e;
}

static void test_roundtrip(void)
{
    reader_bookmark_blob_t blob;
    reader_bookmark_init(&blob);
    CHECK(blob.magic == READER_BOOKMARK_MAGIC);
    CHECK(blob.version == READER_BOOKMARK_VERSION);
    CHECK(blob.count == 0);

    const uint32_t book = reader_book_id("demo.txt");
    reader_bookmark_entry_t e = make(book, 4096, 3);
    CHECK(reader_bookmark_add(&blob, &e) == 1);
    CHECK(blob.count == 1);

    uint8_t buf[READER_BOOKMARK_BLOB_SIZE];
    int n = reader_bookmark_serialize(&blob, buf, sizeof(buf));
    CHECK(n == (int)(READER_BOOKMARK_HEADER_SIZE + READER_BOOKMARK_ENTRY_SIZE));

    reader_bookmark_blob_t restored;
    CHECK(reader_bookmark_parse(buf, (size_t)n, &restored) == 0);
    CHECK(restored.count == 1);
    CHECK(restored.entries[0].book_id == book);
    CHECK(restored.entries[0].offset == 4096);
    CHECK(restored.entries[0].chapter == 3);
    CHECK(reader_bookmark_find(&restored, book, 4096) == 0);
    CHECK(reader_bookmark_find(&restored, book, 4097) == -1);
}

static void test_dedupe_and_remove(void)
{
    reader_bookmark_blob_t blob;
    reader_bookmark_init(&blob);
    const uint32_t book = 42;

    reader_bookmark_entry_t e = make(book, 100, 0);
    CHECK(reader_bookmark_add(&blob, &e) == 1);
    CHECK(reader_bookmark_add(&blob, &e) == 0);  // same page, no duplicate
    CHECK(blob.count == 1);

    CHECK(reader_bookmark_remove(&blob, book, 100) == 1);
    CHECK(blob.count == 0);
    CHECK(reader_bookmark_remove(&blob, book, 100) == 0);  // already gone
}

static void test_per_book_filter_and_order(void)
{
    reader_bookmark_blob_t blob;
    reader_bookmark_init(&blob);

    // Insert out of order, and interleave a second book.
    reader_bookmark_entry_t a = make(1, 500, 2);
    reader_bookmark_entry_t b = make(1, 100, 0);
    reader_bookmark_entry_t c = make(2, 900, 5);
    reader_bookmark_entry_t d = make(1, 300, 1);
    CHECK(reader_bookmark_add(&blob, &a) == 1);
    CHECK(reader_bookmark_add(&blob, &b) == 1);
    CHECK(reader_bookmark_add(&blob, &c) == 1);
    CHECK(reader_bookmark_add(&blob, &d) == 1);

    reader_bookmark_entry_t out[READER_BOOKMARK_MAX];
    int n = reader_bookmark_list(&blob, 1, out, READER_BOOKMARK_MAX);
    CHECK(n == 3);
    CHECK(out[0].offset == 100);
    CHECK(out[1].offset == 300);
    CHECK(out[2].offset == 500);

    n = reader_bookmark_list(&blob, 2, out, READER_BOOKMARK_MAX);
    CHECK(n == 1);
    CHECK(out[0].offset == 900);

    CHECK(reader_bookmark_list(&blob, 99, out, READER_BOOKMARK_MAX) == 0);
    // max caps the copy.
    CHECK(reader_bookmark_list(&blob, 1, out, 2) == 2);
}

static void test_fifo_eviction(void)
{
    reader_bookmark_blob_t blob;
    reader_bookmark_init(&blob);
    for (uint32_t i = 0; i < READER_BOOKMARK_MAX + 4; ++i) {
        reader_bookmark_entry_t e = make(7, i * 10, 0);
        CHECK(reader_bookmark_add(&blob, &e) == 1);
    }
    CHECK(blob.count == READER_BOOKMARK_MAX);
    CHECK(reader_bookmark_find(&blob, 7, 0) == -1);          // oldest evicted
    CHECK(reader_bookmark_find(&blob, 7, (READER_BOOKMARK_MAX + 3) * 10) >= 0);

    uint8_t buf[READER_BOOKMARK_BLOB_SIZE];
    int n = reader_bookmark_serialize(&blob, buf, sizeof(buf));
    CHECK(n == (int)READER_BOOKMARK_BLOB_SIZE);
    reader_bookmark_blob_t restored;
    CHECK(reader_bookmark_parse(buf, (size_t)n, &restored) == 0);
    CHECK(restored.count == READER_BOOKMARK_MAX);
}

static void test_corruption(void)
{
    reader_bookmark_blob_t blob;
    reader_bookmark_init(&blob);
    for (uint32_t i = 0; i < 3; ++i) {
        reader_bookmark_entry_t e = make(9, i * 100, 0);
        reader_bookmark_add(&blob, &e);
    }
    uint8_t buf[READER_BOOKMARK_BLOB_SIZE];
    int n = reader_bookmark_serialize(&blob, buf, sizeof(buf));
    CHECK(n > 0);

    uint8_t bad[READER_BOOKMARK_BLOB_SIZE];
    memcpy(bad, buf, (size_t)n);
    bad[READER_BOOKMARK_HEADER_SIZE + 3] ^= 0x20;  // flip one entry byte
    reader_bookmark_blob_t out;
    CHECK(reader_bookmark_parse(bad, (size_t)n, &out) == -1);
    CHECK(out.count == 0);
    CHECK(out.magic == READER_BOOKMARK_MAGIC);

    memcpy(bad, buf, (size_t)n);
    bad[0] ^= 0xFF;  // wrong magic
    CHECK(reader_bookmark_parse(bad, (size_t)n, &out) == -1);

    memcpy(bad, buf, (size_t)n);
    bad[4] = 99;  // wrong version
    CHECK(reader_bookmark_parse(bad, (size_t)n, &out) == -1);

    CHECK(reader_bookmark_parse(buf, (size_t)n - 1, &out) == -1);
    CHECK(reader_bookmark_parse(buf, 4, &out) == -1);
    CHECK(reader_bookmark_parse(NULL, 0, &out) == -1);
}

static void test_bounds(void)
{
    reader_bookmark_blob_t blob;
    reader_bookmark_init(&blob);
    uint8_t small[8];
    CHECK(reader_bookmark_serialize(&blob, small, sizeof(small)) == -1);
    uint8_t buf[READER_BOOKMARK_BLOB_SIZE];
    CHECK(reader_bookmark_serialize(&blob, buf, READER_BOOKMARK_HEADER_SIZE) ==
          (int)READER_BOOKMARK_HEADER_SIZE);
    CHECK(reader_bookmark_serialize(&blob, buf, READER_BOOKMARK_HEADER_SIZE - 1) == -1);
    CHECK(reader_bookmark_add(NULL, NULL) == -1);
    CHECK(reader_bookmark_list(NULL, 0, NULL, 0) == 0);
}

static void test_remove_book(void)
{
    reader_bookmark_blob_t blob;
    reader_bookmark_init(&blob);
    for (int i = 0; i < 3; ++i) {
        reader_bookmark_entry_t e = {
            .book_id = 11, .offset = (uint32_t)(i * 100), .chapter = 0, .flags = 0
        };
        CHECK(reader_bookmark_add(&blob, &e) == 1);
    }
    reader_bookmark_entry_t other = {
        .book_id = 22, .offset = 50, .chapter = 0, .flags = 0
    };
    CHECK(reader_bookmark_add(&blob, &other) == 1);
    CHECK(blob.count == 4);

    // Deleting a book must compact the table and keep every other book's marks
    // intact, in their original order, with a still-valid CRC.
    CHECK(reader_bookmark_remove_book(&blob, 11) == 3);
    CHECK(blob.count == 1);
    CHECK(blob.entries[0].book_id == 22);
    CHECK(blob.entries[0].offset == 50);
    uint8_t buf[READER_BOOKMARK_BLOB_SIZE];
    int n = reader_bookmark_serialize(&blob, buf, sizeof(buf));
    CHECK(n > 0);
    reader_bookmark_blob_t parsed;
    CHECK(reader_bookmark_parse(buf, (size_t)n, &parsed) == 0);
    CHECK(parsed.count == 1);
    CHECK(parsed.entries[0].offset == 50);

    CHECK(reader_bookmark_remove_book(&blob, 999) == 0);
    CHECK(reader_bookmark_remove_book(NULL, 11) == 0);
}

int main(void)
{
    test_roundtrip();
    test_dedupe_and_remove();
    test_per_book_filter_and_order();
    test_fifo_eviction();
    test_corruption();
    test_bounds();
    test_remove_book();

    if (failures) {
        printf("reader_bookmark: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_bookmark: PASS\n");
    return 0;
}
