// reader_storage.c — see reader_storage.h.
#include "reader_storage.h"

#include <stdio.h>
#include <string.h>

// Approximate UTF-8 sizes of well-known works, used only as yardsticks. They
// are deliberately round: the point is an intuitive "about N more", not a
// byte-exact library catalogue. A "book" is a long work, an "article" a short
// one; each row is chosen for a different remaining-space scale.
typedef struct {
    size_t min_free;       // use this row when free space is at least this big
    const char *book;
    size_t book_bytes;
    const char *article;
    size_t article_bytes;
} storage_reference_t;

static const storage_reference_t REFERENCES[] = {
    // The long-work row needs enough free space for at least one book. The
    // short-poem row is the fallback for everything below that; the old third
    // entry was a byte-for-byte duplicate of it with a lower threshold, so
    // removing it cannot change any hint the page can produce.
    { 2200000u, "《红楼梦》",     2200000u, "《岳阳楼记》", 1080u },
    { 0u,       "《唐诗三百首》",   60000u, "《静夜思》",     60u },
};

int reader_storage_percent(size_t part, size_t total)
{
    if (total == 0) return 0;
    if (part >= total) return 100;
    // Widen before multiplying: a 4 MiB partition times 100 overflows 32 bits
    // on hosts with a 32-bit size_t.
    unsigned long long pct = (unsigned long long)part * 100u / total;
    return pct > 100u ? 100 : (int)pct;
}

// Format a count compactly: 68300 -> "6.8万". Kept in integers so the hint
// needs no floating point on the RISC-V target.
static void format_count(size_t n, char *buf, size_t size)
{
    if (n >= 10000u) {
        size_t wan = n / 10000u;
        size_t tenth = (n % 10000u) / 1000u;
        snprintf(buf, size, "%u.%u万", (unsigned)wan, (unsigned)tenth);
    } else {
        snprintf(buf, size, "%u", (unsigned)n);
    }
}

int reader_storage_hint(size_t free_bytes, char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    // Always leave a valid empty string behind, even when the buffer is too
    // small for the shortest hint. Returning before this line would leave the
    // caller's bytes untouched, making the result depend on the stack.
    out[0] = '\0';
    if (out_size < 16) return -1;

    if (free_bytes == 0) {
        snprintf(out, out_size, "%s", "空间已满，先删除旧书再传新书");
        return (int)strlen(out);
    }

    const storage_reference_t *ref = &REFERENCES[0];
    for (size_t i = 0; i < sizeof(REFERENCES) / sizeof(REFERENCES[0]); ++i) {
        if (free_bytes >= REFERENCES[i].min_free) {
            ref = &REFERENCES[i];
            break;
        }
    }

    size_t books = free_bytes / ref->book_bytes;
    size_t articles = free_bytes / ref->article_bytes;
    if (books == 0 && articles == 0) {
        snprintf(out, out_size, "%s", "剩余空间不多，建议先删除旧书");
        return (int)strlen(out);
    }

    char bn[16], an[16];
    format_count(books, bn, sizeof(bn));
    format_count(articles, an, sizeof(an));
    int n;
    if (books == 0) {
        // Too little space for a whole book: report the short works only
        // rather than printing a misleading "0 本《...》".
        n = snprintf(out, out_size, "约可再存 %s篇%s", an, ref->article);
    } else {
        // No space before 本/篇: "1.1万篇" reads correctly, while a space
        // would split the 万 unit from its counter.
        n = snprintf(out, out_size, "约可再存 %s本%s，或 %s篇%s",
                     bn, ref->book, an, ref->article);
    }
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return n;
}
