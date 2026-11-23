// reader_ui.h — redesigned paper-and-ink screens for the eBook reader.
//
// This is NOT the baseline demo shell: no test menu, no pixel-art mascot, no
// ui_pixel_* helpers. Every screen is driven entirely by state supplied by
// reader_app.c; rendering is a pure function of that state. All functions must
// be called from the LVGL task or while holding bsp_lvgl_lock().
#pragma once

#include "reader_store.h"
#include "reader_transfer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    READER_SCREEN_LIBRARY = 0,
    READER_SCREEN_READER,
    READER_SCREEN_TOC,
    READER_SCREEN_SETTINGS,
    READER_SCREEN_FONT_PICKER,
    READER_SCREEN_TRANSFER,
    READER_SCREEN_FINISHED,
    READER_SCREEN_STORAGE,
    READER_SCREEN_CONFIRM,
    READER_SCREEN_BACKLIGHT,  // brightness ruler (same shape as the font picker)
    READER_SCREEN_AUTOPAGE,   // auto page-turn interval ruler
    READER_SCREEN_COVER,   // first-open title card, shown during the load pause
} reader_screen_t;

#include "reader_settings.h"

// Bytes for one bookmark label (UTF-8, NUL included). A label is a short
// excerpt of the book text at the bookmark offset, never a chapter name.
#define READER_BOOKMARK_TITLE_SIZE 40

void reader_ui_init(void);
void reader_ui_deinit(void);

// Selects the light (paper) or dark palette. The next full render rebuilds
// every widget with the new colours; in-place refresh calls do not recolour.
void reader_ui_set_dark(bool dark);

// Overlay for a long-running book-open operation. Replaced on the next render.
void reader_ui_update_loading(const char *title, const char *phase, unsigned percent);

// Transient reader-footer message (e.g. "已添加书签"). An empty string restores
// the default key hint on the next full render.
void reader_ui_set_hint(const char *text);

// `battery` is a pre-formatted short string (e.g. "99%", "~85%" or "电量未知").
// The application owns the formatting because it also owns the fallback policy;
// passing text keeps every screen free of hardware-specific branches.
//
// Full redraws. Each one replaces the previous screen, so no stale widget can
// be referenced after navigation.
void reader_ui_render_library(const reader_book_info_t *books, int count,
                              int selected, const char *battery);
// `percent` is the share of the book that lies behind the current page, so the
// last page reports 100. It is computed by the application, not here, because
// only the paginator knows where the page ends. `page`/`page_count` are the
// 1-based page number and the book's total page count under the CURRENT
// geometry; both 0 means "not known yet" and the footer then shows percent only.
// `immersive` strips every chrome element (title, chapter, progress, battery,
// key hints) and uses the larger full-height line budget; the ONLY exception is
// a transient bookmark toast.
void reader_ui_render_reader(const char *title, const char *chapter,
                             const char *page_text, int font_size,
                             int percent, int page, int page_count,
                             const char *battery, int auto_mode, int immersive);
// TOC screen with three tabs. mode: 0 = chapters, 1 = bookmarks, 2 = page
// ranges. focus: 0 = the tab row, 1 = the list. selected indexes the active
// list. Each tab carries its own count, so no separate caption sits above them.
// `mark_titles` holds one excerpt label per bookmark (see reader_excerpt);
// bookmark rows show the text the reader marked, not a chapter heading.
// `page_groups` is how many READER_PAGEMAP_GROUP-sized ranges the page map has
// (0 when the map is unavailable) and `page_count` is the total page count; the
// page tab turns them into "1—10页" rows for books whose chapters are unreliable.
void reader_ui_render_toc(const reader_book_index_t *index,
                          int mark_count, const char *const *mark_titles,
                          int page_groups, int page_count,
                          int mode, int focus, int selected,
                          const char *battery);
// `auto_seconds` is the auto page-turn interval (0 = off); the row opens its own
// ruler page, exactly like 正文字号 and 屏幕亮度 do.
void reader_ui_render_settings(int selected, int font_size, int backlight,
                               int auto_seconds, int idle_seconds, int dark,
                               int immersive, const char *battery);
// Brightness ruler: a big percentage, the ladder as dots, and the actual change
// applied live while the reader scrolls it.
void reader_ui_render_backlight_picker(int backlight, const char *battery);
// Auto page-turn ruler. The page also carries the hint about the reading-screen
// shortcut, because that is where the reader looks for it.
void reader_ui_render_autopage_picker(int auto_seconds, const char *battery);
// Storage page: remaining space, used/free percentages as a progress bar,
// totals, and one generated "about N more works" hint.
void reader_ui_render_storage(size_t total, size_t used, size_t free_bytes,
                              const char *hint, const char *battery);
// Font-size picker: a live preview rendered with exactly the reading page's
// font and line pitch, plus a 12..20 step ruler and the numeric size. No
// caption line — the sample and the number are the whole explanation.
void reader_ui_render_font_picker(int font_size, const char *battery);
// End-of-book confirmation. Shows a large centred "完", the book title as a
// quiet subtitle, and a single wrapped key chip; the actual return to the
// shelf is a second key press.
void reader_ui_render_finished(const char *book_title);
// Title card (page 0): same centred typography as the 完 page, but the large
// word is the book's title and the subtitle is "N字 · X KB". `flash_bytes` is
// the compressed on-flash footprint (not the uncompressed byte length used for
// progress), so the number matches the shelf's "X KB" size display. Rendered
// before the chapter index is built, so the reader sees something during the
// load pause; char_count may be 0 in that first frame. Pressing UP on the first
// text page comes back here, and DOWN/OK resumes.
void reader_ui_render_cover(const char *book_title, size_t char_count,
                            size_t flash_bytes);
// Destructive-action confirmation (delete a bookmark or a book). `question`
// is the short prompt, `item` the name/excerpt of what would be removed.
void reader_ui_render_confirm(const char *question, const char *item,
                              const char *battery);
void reader_ui_render_transfer(const reader_transfer_status_t *status,
                               size_t total, size_t used, size_t free_bytes,
                               const char *battery);

// Lightweight in-place updates used by the 1 s tick timer (no screen rebuild).
void reader_ui_refresh_transfer(const reader_transfer_status_t *status,
                                size_t total, size_t used, size_t free_bytes);
void reader_ui_set_battery(const char *text);
