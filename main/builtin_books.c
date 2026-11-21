// builtin_books.c — one protected guide and one full-length novel.
//
// The bodies are compiled as zdx-compressed images by tools/gen_builtin_books.py
// (see main/builtin_books_data.c). The canonical UTF-8 text lives in
// assets/books/*.txt and is scanned by tools/gen_reader_font.py for glyph
// coverage. Do not hand-edit the generated data; regenerate with:
//   PYTHONDONTWRITEBYTECODE=1 python3 tools/gen_builtin_books.py
#include "builtin_books.h"
#include "builtin_books_data.h"

const reader_builtin_book_t g_builtin_books[] = {
    { READER_BUILTIN_GUIDE_ID, "掌小读使用指南", g_builtin_zdx_guide,
      sizeof(g_builtin_zdx_guide),
      READER_ZDX_GUIDE_ORIG_SIZE, READER_ZDX_GUIDE_TEXT_CHARS },
    { "builtin:ginga", "银河铁道之夜", g_builtin_zdx_ginga,
      sizeof(g_builtin_zdx_ginga),
      READER_ZDX_GINGA_ORIG_SIZE, READER_ZDX_GINGA_TEXT_CHARS },
};

const int g_builtin_book_count =
    (int)(sizeof(g_builtin_books) / sizeof(g_builtin_books[0]));

int reader_builtin_open(const reader_builtin_book_t *book, reader_zdx_t *z)
{
    if (!book || !z || !book->zdx) return -1;
    return reader_zdx_open_mem(z, book->zdx, book->zdx_len);
}
