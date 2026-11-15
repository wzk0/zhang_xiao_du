// reader_text.c — pure text engine. See reader_text.h for the contract.
#include "reader_text.h"

#include <string.h>

static int in_range(uint32_t cp, uint32_t lo, uint32_t hi)
{
    return cp >= lo && cp <= hi;
}

uint32_t reader_utf8_decode(const char *s, size_t len, size_t *consumed)
{
    if (!s || len == 0) {
        if (consumed) *consumed = 0;
        return READER_INVALID_CP;
    }
    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[0];
    size_t n;
    uint32_t cp;

    if (c < 0x80) {
        if (consumed) *consumed = 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2; cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3; cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4; cp = c & 0x07u;
    } else {
        if (consumed) *consumed = 1;
        return READER_INVALID_CP;
    }
    if (len < n) {
        if (consumed) *consumed = 1;
        return READER_INVALID_CP;
    }
    for (size_t i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            if (consumed) *consumed = 1;
            return READER_INVALID_CP;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    // Reject overlong forms, surrogates and out-of-range scalars.
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
        (n == 4 && cp < 0x10000) || in_range(cp, 0xD800, 0xDFFF) ||
        cp > 0x10FFFF) {
        if (consumed) *consumed = 1;
        return READER_INVALID_CP;
    }
    if (consumed) *consumed = n;
    return cp;
}

int reader_is_wide(uint32_t cp)
{
    if (in_range(cp, 0x1100, 0x115F)) return 1;       // Hangul Jamo
    if (in_range(cp, 0x2E80, 0x303E)) return 1;       // CJK radicals, punctuation
    if (in_range(cp, 0x3041, 0x33FF)) return 1;       // Kana, bopomofo, CJK symbols
    if (in_range(cp, 0x3400, 0x4DBF)) return 1;       // CJK ext A
    if (in_range(cp, 0x4E00, 0x9FFF)) return 1;       // CJK unified
    if (in_range(cp, 0xA000, 0xA4CF)) return 1;       // Yi
    if (in_range(cp, 0xAC00, 0xD7A3)) return 1;       // Hangul syllables
    if (in_range(cp, 0xF900, 0xFAFF)) return 1;       // CJK compatibility
    if (in_range(cp, 0xFE10, 0xFE6F)) return 1;       // vertical forms
    if (in_range(cp, 0xFF00, 0xFF60)) return 1;       // full-width forms
    if (in_range(cp, 0xFFE0, 0xFFE6)) return 1;
    if (in_range(cp, 0x20000, 0x3FFFD)) return 1;     // CJK ext B and beyond
    return 0;
}

// Punctuation that must not start a line. When a line is full we let one such
// character overflow rather than pushing it to the next line (kinsoku shori).
static int is_line_start_banned(uint32_t cp)
{
    static const uint32_t banned[] = {
        0x0021, 0x0029, 0x002C, 0x002E, 0x003A, 0x003B, 0x003F, 0x0027,
        0x0022, 0x3001, 0x3002, 0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A,
        0xFF1B, 0xFF1F, 0x2019, 0x201D, 0x3009, 0x300B, 0x300D, 0x3011,
        0x3015, 0x300A, 0x2026, 0x30FB, 0xFF05,
    };
    for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); ++i) {
        if (cp == banned[i]) return 1;
    }
    return 0;
}

int reader_paginate(const char *text, size_t text_len, size_t start,
                    int cols_units, int max_lines,
                    char *out, size_t out_size, reader_page_t *page)
{
    if (!text || !out || !page || out_size < 8 || cols_units < 4 || max_lines < 1 ||
        start > text_len) {
        return -1;
    }

    size_t pos = start;
    size_t used = 0;
    int line_units = 0;
    int lines = 0;
    out[0] = '\0';

    while (pos < text_len && lines < max_lines) {
        size_t consumed = 0;
        uint32_t cp = reader_utf8_decode(text + pos, text_len - pos, &consumed);
        if (consumed == 0) break;

        // Leave room for a character, a possible '\n' and the terminating NUL.
        if (used + consumed + 2 >= out_size) break;

        if (cp == '\r') {                  // CRLF: drop CR, keep the LF below
            pos += consumed;
            continue;
        }
        if (cp == '\n') {
            out[used++] = '\n';
            ++lines;
            line_units = 0;
            pos += consumed;
            continue;
        }
        if (cp < 0x20 && cp != '\t') {     // other controls: skip, never draw
            pos += consumed;
            continue;
        }

        int units = reader_is_wide(cp) ? 2 : 1;
        if (cp == '\t') units = 2;

        if (line_units + units > cols_units && !is_line_start_banned(cp)) {
            out[used++] = '\n';
            ++lines;
            line_units = 0;
            if (lines >= max_lines) break;  // leave this character for next page
        }

        if (cp == '\t' || cp == READER_INVALID_CP) {
            out[used++] = '?';
        } else {
            memcpy(out + used, text + pos, consumed);
            used += consumed;
        }
        line_units += units;
        pos += consumed;
    }

    // A page must always advance, otherwise a single over-wide character would
    // loop forever. This only triggers with a degenerate (too small) buffer.
    if (pos == start && start < text_len) {
        size_t consumed = 0;
        (void)reader_utf8_decode(text + pos, text_len - pos, &consumed);
        pos += consumed ? consumed : 1;
    }

    // A trailing line without a newline still occupies one line of the page.
    // This never exceeds max_lines: the loop stops as soon as a wrap fills the
    // last line, and in that case the output already ends with '\n'.
    if (used > 0 && out[used - 1] != '\n') ++lines;

    out[used] = '\0';
    page->start = start;
    page->next = pos;
    page->length = used;
    page->lines = lines;
    page->is_last = pos >= text_len;
    return 0;
}

int reader_excerpt(const char *text, size_t text_len, int max_units,
                   char *out, size_t out_size)
{
    if (!text || !out || out_size < 4 || max_units < 1) return -1;
    out[0] = '\0';

    static const char ELLIPSIS[] = "…";
    const size_t ellipsis_len = sizeof(ELLIPSIS) - 1;
    size_t pos = 0, used = 0;
    int units = 0;
    int truncated = 0;

    // Skip blank lines and indentation so a bookmark on an empty line still
    // shows the sentence the reader was actually looking at.
    while (pos < text_len) {
        size_t consumed = 0;
        uint32_t cp = reader_utf8_decode(text + pos, text_len - pos, &consumed);
        if (consumed == 0) break;
        if (cp != ' ' && cp != '\t' && cp != '\r' && cp != '\n' &&
            cp != 0x3000 && !(cp < 0x20) && cp != READER_INVALID_CP) {
            break;
        }
        pos += consumed;
    }

    while (pos < text_len) {
        size_t consumed = 0;
        uint32_t cp = reader_utf8_decode(text + pos, text_len - pos, &consumed);
        if (consumed == 0) break;
        if (cp == '\n' || cp == '\r') break;  // one line only
        if (cp == READER_INVALID_CP || cp < 0x20) {
            pos += consumed;
            continue;
        }
        int w = reader_is_wide(cp) ? 2 : 1;
        if (units + w > max_units) { truncated = 1; break; }
        // Reserve room for a possible ellipsis and the terminating NUL.
        if (used + consumed + ellipsis_len + 1 > out_size) { truncated = 1; break; }
        memcpy(out + used, text + pos, consumed);
        used += consumed;
        units += w;
        pos += consumed;
    }

    if (truncated && used + ellipsis_len + 1 <= out_size) {
        memcpy(out + used, ELLIPSIS, ellipsis_len);
        used += ellipsis_len;
    }
    out[used] = '\0';
    return (int)used;
}

int reader_chapter_match(const char *line, size_t len)
{
    if (!line || len == 0 || len > 512) return 0;

    // Trim leading ASCII spaces and full-width spaces (U+3000, EF BC A0 is
    // U+FF0F; U+3000 is E3 80 80).
    size_t i = 0;
    while (i < len) {
        if (line[i] == ' ' || line[i] == '\t') { ++i; continue; }
        if (i + 2 < len && (unsigned char)line[i] == 0xE3 &&
            (unsigned char)line[i + 1] == 0x80 && (unsigned char)line[i + 2] == 0x80) {
            i += 3; continue;
        }
        break;
    }
    size_t rest = len - i;
    if (rest == 0) return 0;
    const char *p = line + i;

    static const char *latin[] = { "Chapter", "CHAPTER", "chapter", "Part", "PART" };
    for (size_t k = 0; k < sizeof(latin) / sizeof(latin[0]); ++k) {
        size_t n = strlen(latin[k]);
        if (rest >= n && memcmp(p, latin[k], n) == 0) return 1;
    }

    // Chinese headings: 第...章/节/回/卷, or 卷..., within the first 24 bytes.
    const char *di = "\xE7\xAC\xAC";  // 第
    const char *juan = "\xE5\x8D\xB7";  // 卷
    if (rest >= 3 && memcmp(p, di, 3) == 0) {
        size_t scan = rest < 24 ? rest : 24;
        for (size_t k = 3; k + 2 < scan; ++k) {
            const char *tail[] = {
                "\xE7\xAB\xA0",  // 章
                "\xE8\x8A\x82",  // 节
                "\xE5\x9B\x9E",  // 回
                "\xE5\x8D\xB7",  // 卷
            };
            for (size_t t = 0; t < 4; ++t) {
                if (memcmp(p + k, tail[t], 3) == 0) return 1;
            }
        }
    }
    if (rest >= 3 && memcmp(p, juan, 3) == 0) return 1;
    return 0;
}

int reader_scan_charset(const char *text, size_t text_len,
                        int (*is_supported)(uint32_t cp, void *ctx), void *ctx,
                        reader_charset_report_t *report)
{
    if (!text || !is_supported || !report) return -1;
    memset(report, 0, sizeof(*report));

    size_t pos = 0;
    while (pos < text_len) {
        size_t consumed = 0;
        uint32_t cp = reader_utf8_decode(text + pos, text_len - pos, &consumed);
        if (consumed == 0) break;
        pos += consumed;
        if (cp == READER_INVALID_CP || cp < 0x20 || cp == 0x7F) continue;
        if (cp < 0x80) continue;  // ASCII is always covered
        ++report->total;
        if (!is_supported(cp, ctx)) {
            ++report->unsupported;
            if (report->sample_count < 8) {
                report->sample[report->sample_count++] = cp;
            }
        }
    }
    return 0;
}
