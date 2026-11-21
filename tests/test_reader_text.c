// Host tests for the pure text engine.
// Build: cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_reader_text.c
//        main/reader_text.c -o /tmp/test_reader_text
#include "reader_text.h"

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

static void test_utf8(void)
{
    size_t n = 0;
    CHECK(reader_utf8_decode("A", 1, &n) == 'A' && n == 1);
    CHECK(reader_utf8_decode("\xC3\xA9", 2, &n) == 0xE9 && n == 2);
    CHECK(reader_utf8_decode("\xE4\xB8\xAD", 3, &n) == 0x4E2D && n == 3);
    CHECK(reader_utf8_decode("\xF0\x9F\x98\x80", 4, &n) == 0x1F600 && n == 4);
    // Truncated and malformed input consumes one byte and reports invalid.
    CHECK(reader_utf8_decode("\xE4\xB8", 2, &n) == READER_INVALID_CP && n == 1);
    CHECK(reader_utf8_decode("\xE4\x28\xA1", 3, &n) == READER_INVALID_CP && n == 1);
    CHECK(reader_utf8_decode("\xC0\xAF", 2, &n) == READER_INVALID_CP && n == 1); // overlong
}

static void test_width(void)
{
    CHECK(reader_is_wide(0x4E2D) == 1);   // 中
    CHECK(reader_is_wide(0xFF0C) == 1);   // ，
    CHECK(reader_is_wide(0x3000) == 1);   // ideographic space
    CHECK(reader_is_wide(0x0041) == 0);
    CHECK(reader_is_wide(0x00E9) == 0);
    CHECK(reader_is_wide(0x2014) == 0);   // em dash: half-width in our model
}

// Mimics a font inventory: GB2312-level CJK plus two common punctuation marks.
static int supported_up_to_9fff(uint32_t cp, void *ctx)
{
    (void)ctx;
    if (cp >= 0x4E00 && cp <= 0x9F00) return 1;
    if (cp == 0xFF0C || cp == 0x3002) return 1;
    return 0;
}

static void validate_utf8(const char *s)
{
    size_t len = strlen(s);
    size_t pos = 0;
    while (pos < len) {
        size_t consumed = 0;
        uint32_t cp = reader_utf8_decode(s + pos, len - pos, &consumed);
        CHECK(cp != READER_INVALID_CP);
        CHECK(consumed > 0);
        pos += consumed ? consumed : 1;
    }
    CHECK(pos == len);
}

static void test_pagination_ascii(void)
{
    const char *text = "abcdefghijklmnopqrstuvwxyz";
    char out[256];
    reader_page_t p;
    CHECK(reader_paginate(text, strlen(text), 0, 10, 2, out, sizeof(out), &p) == 0);
    CHECK(strcmp(out, "abcdefghij\nklmnopqrst\n") == 0);
    CHECK(p.next == 20);
    CHECK(p.lines == 2);
    CHECK(p.is_last == 0);
    validate_utf8(out);

    CHECK(reader_paginate(text, strlen(text), p.next, 10, 2, out, sizeof(out), &p) == 0);
    CHECK(strcmp(out, "uvwxyz") == 0);
    CHECK(p.is_last == 1);
}

static void test_pixel_width_preserves_text(void)
{
    const char *text = "WWWWWWWWWWWWWWWW";
    uint16_t advances[95] = {0};
    advances['W' - 32] = 225; // The shipped 16 px glyph rounds to 14 px.
    char out[64];
    reader_page_t page;
    CHECK(reader_paginate_fit(text, strlen(text), 0, 24, 1, 16, 204, advances,
                              out, sizeof(out), &page) == 0);
    CHECK(strcmp(out, "WWWWWWWWWWWWWW\n") == 0);
    CHECK(page.next == 14 && !page.is_last);
    CHECK(reader_paginate_fit(text, strlen(text), page.next, 24, 1, 16, 204, advances,
                              out, sizeof(out), &page) == 0);
    CHECK(strcmp(out, "WW") == 0 && page.is_last);
}

static void test_pagination_cjk(void)
{
    // 12 Chinese characters, 3 bytes each. 4 wide chars fit in 8 units.
    const char *text = "一二三四五六七八九十甲乙丙";
    char out[256];
    reader_page_t p;
    CHECK(reader_paginate(text, strlen(text), 0, 8, 3, out, sizeof(out), &p) == 0);
    // 4 chars per line, 3 lines.
    CHECK(strcmp(out, "一二三四\n五六七八\n九十甲乙\n") == 0);
    CHECK(p.next == 36);
    CHECK(p.lines == 3);
    CHECK(p.is_last == 0);
    validate_utf8(out);

    CHECK(reader_paginate(text, strlen(text), p.next, 8, 3, out, sizeof(out), &p) == 0);
    CHECK(strcmp(out, "丙") == 0);
    CHECK(p.is_last == 1);
}

static void test_pagination_no_utf8_split(void)
{
    const char *text = "中文abc中文中文中文中文中文中文";
    char out[128];
    reader_page_t p;
    size_t offset = 0;
    int pages = 0;
    while (reader_paginate(text, strlen(text), offset, 9, 2, out, sizeof(out), &p) == 0) {
        validate_utf8(out);
        ++pages;
        CHECK(p.next > offset || p.is_last);
        if (p.next == offset) break;
        offset = p.next;
        if (p.is_last || pages > 32) break;
    }
    CHECK(offset == strlen(text));
    CHECK(pages > 1);
}

static void test_pagination_newlines_and_buffer(void)
{
    const char *text = "第一章\n正文开始\n第二章\n结束";
    char out[64];
    reader_page_t p;
    CHECK(reader_paginate(text, strlen(text), 0, 20, 5, out, sizeof(out), &p) == 0);
    CHECK(strcmp(out, "第一章\n正文开始\n第二章\n结束") == 0);
    CHECK(p.is_last == 1);
    CHECK(p.lines == 4);

    // A tiny output buffer must not overflow and must still terminate.
    char tiny[8];
    CHECK(reader_paginate(text, strlen(text), 0, 20, 5, tiny, sizeof(tiny), &p) == 0);
    CHECK(strlen(tiny) < sizeof(tiny));
    CHECK(p.next > 0);

    // Invalid arguments are rejected.
    CHECK(reader_paginate(NULL, 0, 0, 10, 2, out, sizeof(out), &p) == -1);
    CHECK(reader_paginate(text, strlen(text), 0, 10, 2, out, 4, &p) == -1);
    CHECK(reader_paginate(text, strlen(text), 999, 10, 2, out, sizeof(out), &p) == -1);
}

static void test_punctuation_kinsoku(void)
{
    // Closing punctuation must not be pushed to the start of the next line.
    const char *text = "一二三四，五六七八";  // 4 chars + full-width comma
    char out[128];
    reader_page_t p;
    // 8 units = 4 wide chars; the comma is the 5th wide char and would wrap.
    CHECK(reader_paginate(text, strlen(text), 0, 8, 2, out, sizeof(out), &p) == 0);
    const char *nl = strchr(out, '\n');
    if (nl) {
        size_t used = 0;
        uint32_t cp = reader_utf8_decode(nl + 1, strlen(nl + 1), &used);
        CHECK(cp != 0xFF0C);  // never a full-width comma at line start
    }
    validate_utf8(out);
}

static void validate_excerpt_utf8(const char *s)
{
    size_t len = strlen(s), pos = 0;
    while (pos < len) {
        size_t n = 0;
        uint32_t cp = reader_utf8_decode(s + pos, len - pos, &n);
        CHECK(cp != READER_INVALID_CP);
        CHECK(n > 0);
        pos += n ? n : 1;
    }
}

static void test_excerpt(void)
{
    char out[40];
    const char *text = "\n\n  床前明月光，疑是地上霜。\n举头望明月，低头思故乡。";

    // Leading blank lines and indentation are skipped; the label is the SENTENCE
    // at the bookmark, not a chapter heading. A narrow budget adds an ellipsis.
    int n = reader_excerpt(text, strlen(text), 8, out, sizeof(out));
    CHECK(n > 0);
    CHECK(strcmp(out, "床前明月…") == 0);
    CHECK(strchr(out, '\n') == NULL);
    validate_excerpt_utf8(out);

    // A wide budget reaches the line break, so no ellipsis is needed.
    n = reader_excerpt(text, strlen(text), 40, out, sizeof(out));
    CHECK(n > 0);
    CHECK(strcmp(out, "床前明月光，疑是地上霜。") == 0);
    CHECK(strchr(out, '\n') == NULL);

    // A tiny buffer must never split a 3-byte Hanzi and stays NUL-terminated.
    char tiny[8];
    n = reader_excerpt("中文中文中文", strlen("中文中文中文"), 20, tiny,
                       sizeof(tiny));
    CHECK(n > 0 && (size_t)n < sizeof(tiny));
    validate_excerpt_utf8(tiny);

    // Blank-only, empty and invalid inputs.
    CHECK(reader_excerpt("  \n\t ", 5, 8, out, sizeof(out)) == 0);
    CHECK(reader_excerpt("", 0, 8, out, sizeof(out)) == 0);
    CHECK(reader_excerpt(NULL, 0, 8, out, sizeof(out)) == -1);
    CHECK(reader_excerpt(text, strlen(text), 0, out, sizeof(out)) == -1);
    CHECK(reader_excerpt(text, strlen(text), 8, NULL, 0) == -1);
}

static void test_chapters(void)
{
    CHECK(reader_chapter_match("第一章 初见", strlen("第一章 初见")) == 1);
    CHECK(reader_chapter_match("第十二回 相逢", strlen("第十二回 相逢")) == 1);
    CHECK(reader_chapter_match("卷三", strlen("卷三")) == 1);
    CHECK(reader_chapter_match("Chapter 2 The Sea", strlen("Chapter 2 The Sea")) == 1);
    CHECK(reader_chapter_match("  第三章", strlen("  第三章")) == 1);
    CHECK(reader_chapter_match("他缓缓地说道。", strlen("他缓缓地说道。")) == 0);
    CHECK(reader_chapter_match("", 0) == 0);
    CHECK(reader_chapter_match(NULL, 0) == 0);
}

static void test_charset_scan(void)
{
    const char *text = "中文ABC，。龘";  // 龘 U+9F98 is outside the test set
    reader_charset_report_t r;
    CHECK(reader_scan_charset(text, strlen(text), supported_up_to_9fff, NULL, &r) == 0);
    CHECK(r.total == 5);          // 中文，。龘
    CHECK(r.unsupported == 1);    // only 龘
    CHECK(r.sample_count == 1);
    CHECK(r.sample[0] == 0x9F98);

    CHECK(reader_scan_charset(text, strlen(text), supported_up_to_9fff, NULL, NULL) == -1);
}

int main(void)
{
    test_utf8();
    test_width();
    test_pagination_ascii();
    test_pixel_width_preserves_text();
    test_pagination_cjk();
    test_pagination_no_utf8_split();
    test_pagination_newlines_and_buffer();
    test_punctuation_kinsoku();
    test_excerpt();
    test_chapters();
    test_charset_scan();

    if (failures) {
        printf("reader_text: %d check(s) failed\n", failures);
        return 1;
    }
    printf("reader_text: PASS\n");
    return 0;
}
