// reader_store.c — see reader_store.h.
#include "reader_store.h"

#include "builtin_books.h"
#include "reader_bookmark.h"
#include "reader_fonts.h"
#include "reader_progress.h"
#include "reader_text.h"

#include "dirent.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sys/stat.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "reader_store";
static const char *BOOKS_BASE = "/books";
static const char *BOOKS_LABEL = "books";
static const char *NVS_NAMESPACE = "reader";
static const char *NVS_PROGRESS_KEY = "progress";
static const char *NVS_BOOKMARK_KEY = "bm";
// Bit i (i >= 1) set means g_builtin_books[i] was deleted from the shelf.
// Bit 0 is the guide, which is never hidden. One NVS byte covers every sample;
// with only one deletable built-in today, bit 1 is the one in use.
static const char *NVS_HIDDEN_KEY = "hidden";

static bool s_spiffs_mounted;
static uint8_t s_hidden_builtins;
static nvs_handle_t s_nvs;
static bool s_nvs_open;
static reader_progress_blob_t s_progress;
static reader_bookmark_blob_t s_bookmarks;
static SemaphoreHandle_t s_progress_lock;

static void bookmarks_load_locked(void);
static void bookmarks_save_locked(void);
static FILE *s_upload_fp;
static char s_upload_target[READER_BOOK_ID_MAX];
static SemaphoreHandle_t s_upload_lock;

static void lock_init(void)
{
    if (!s_progress_lock) s_progress_lock = xSemaphoreCreateMutex();
    if (!s_upload_lock) s_upload_lock = xSemaphoreCreateMutex();
}

static void book_path(const char *id, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", BOOKS_BASE, id);
}

static void tmp_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", BOOKS_BASE, READER_UPLOAD_TMP_NAME);
}

// Keep a filename safe for SPIFFS: no path separators, no leading/double dots,
// no control characters. UTF-8 bytes (Chinese names) are preserved.
static void sanitize_name(const char *in, char *out, size_t out_size)
{
    const size_t limit = 32;  // leaves room for the ".txt" suffix
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < limit; ++p) {
        unsigned char c = *p;
        // Also reject characters that could break a JSON/HTML response.
        if (c == '/' || c == '\\' || c == ':' || c < 0x20 || c == 0x7F ||
            c == '"' || c == '\'' || c == '<' || c == '>' || c == '&') continue;
        if (c == '.' && (o == 0 || (o > 0 && out[o - 1] == '.'))) continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    if (o == 0) snprintf(out, out_size, "book");

    size_t len = strlen(out);
    if (len < 4 || strcmp(out + len - 4, ".txt") != 0) {
        if (len + 4 < out_size) strcat(out, ".txt");
    }
}

static void load_progress(void)
{
    reader_progress_init(&s_progress);
    if (!s_nvs_open) return;
    uint8_t buf[READER_PROGRESS_BLOB_SIZE];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(s_nvs, NVS_PROGRESS_KEY, buf, &len);
    if (err == ESP_OK) {
        if (reader_progress_parse(buf, len, &s_progress) != 0) {
            ESP_LOGW(TAG, "进度记录校验失败，使用空记录");
            reader_progress_init(&s_progress);
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取进度失败: %s", esp_err_to_name(err));
    }
}

esp_err_t reader_store_init(void)
{
    lock_init();
    esp_err_t err;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = BOOKS_BASE,
        .partition_label = BOOKS_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,  // a merged 0x0 flash leaves 0xFF here
    };
    err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(err));
        return err;
    }
    s_spiffs_mounted = true;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败，进度将不保存: %s", esp_err_to_name(err));
    } else {
        s_nvs_open = true;
    }
    if (s_nvs_open) {
        uint8_t mask = 0;
        if (nvs_get_u8(s_nvs, NVS_HIDDEN_KEY, &mask) == ESP_OK) {
            s_hidden_builtins = mask;
        }
    }
    load_progress();
    bookmarks_load_locked();

    size_t total = 0, used = 0;
    if (reader_store_space(&total, &used, NULL) == ESP_OK) {
        ESP_LOGI(TAG, "书籍分区: 总计 %u B, 已用 %u B", (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

esp_err_t reader_store_space(size_t *total, size_t *used, size_t *free_bytes)
{
    if (!s_spiffs_mounted) return ESP_ERR_INVALID_STATE;
    size_t t = 0, u = 0;
    esp_err_t err = esp_spiffs_info(BOOKS_LABEL, &t, &u);
    if (err != ESP_OK) return err;
    if (total) *total = t;
    if (used) *used = u;
    if (free_bytes) *free_bytes = u <= t ? t - u : 0;
    return ESP_OK;
}

int reader_store_list(reader_book_info_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int count = 0;

    for (int i = 0; i < g_builtin_book_count && count < max; ++i) {
        // Deleted samples stay in Flash but disappear from the shelf. The
        // protected guide (index 0) is never filtered out.
        if (i > 0 && i < 8 && (s_hidden_builtins & (1u << i))) continue;
        reader_book_info_t *b = &out[count++];
        snprintf(b->id, sizeof(b->id), "%s", g_builtin_books[i].id);
        snprintf(b->title, sizeof(b->title), "%s", g_builtin_books[i].title);
        b->size = (uint32_t)strlen(g_builtin_books[i].text);
        b->builtin = true;
    }

    if (!s_spiffs_mounted) return count;
    DIR *dir = opendir(BOOKS_BASE);
    if (!dir) return count;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;  // skip .upload.tmp and hidden files
        size_t nlen = strlen(name);
        if (nlen < 4 || strcmp(name + nlen - 4, ".txt") != 0) continue;

        char path[READER_BOOK_ID_MAX + 16];
        book_path(name, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) != 0) continue;

        // A name that does not fit the id field could never be reopened for
        // reading, so skip it instead of silently truncating the id.
        if (nlen >= sizeof(out[count].id)) continue;

        reader_book_info_t *b = &out[count++];
        snprintf(b->id, sizeof(b->id), "%.*s",
                 (int)(sizeof(b->id) - 1), name);
        snprintf(b->title, sizeof(b->title), "%.*s",
                 (int)(nlen - 4), name);  // display without .txt
        b->size = (uint32_t)st.st_size;
        b->builtin = false;
    }
    closedir(dir);
    return count;
}

int reader_store_read(const char *id, size_t offset, char *buf, size_t len)
{
    if (!id || !buf || len == 0) return 0;

    if (strncmp(id, "builtin:", 8) == 0) {
        for (int i = 0; i < g_builtin_book_count; ++i) {
            if (strcmp(id, g_builtin_books[i].id) != 0) continue;
            const char *text = g_builtin_books[i].text;
            size_t total = strlen(text);
            if (offset >= total) return 0;
            size_t n = total - offset;
            if (n > len) n = len;
            memcpy(buf, text + offset, n);
            return (int)n;
        }
        return 0;
    }

    char path[READER_BOOK_ID_MAX + 16];
    book_path(id, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    size_t n = fread(buf, 1, len, fp);
    fclose(fp);
    return (int)n;
}

// Truncate a line to a title without splitting a UTF-8 character.
static void copy_title(const char *line, size_t len, char *dst, size_t dst_size)
{
    size_t used = 0, pos = 0;
    while (pos < len && used + 5 < dst_size) {
        size_t consumed = 0;
        (void)reader_utf8_decode(line + pos, len - pos, &consumed);
        if (consumed == 0 || used + consumed + 1 >= dst_size) break;
        memcpy(dst + used, line + pos, consumed);
        used += consumed;
        pos += consumed;
    }
    dst[used] = '\0';
}

esp_err_t reader_store_build_index(const char *id, reader_book_index_t *out)
{
    if (!id || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    // A 1 KiB window cuts the number of streaming reads by 4x on a long
    // novel; static storage keeps it off the 6 KiB input task's stack.
    static char chunk[1024];
    char line[192];
    size_t line_len = 0;
    size_t line_start = 0;
    size_t absolute = 0;

    while (out->count < READER_MAX_CHAPTERS) {
        int n = reader_store_read(id, absolute, chunk, sizeof(chunk));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            char c = chunk[i];
            size_t pos = absolute + (size_t)i;
            // One UTF-8 code point per leading byte (ASCII included), minus
            // ASCII blanks and a leading UTF-8 BOM. This is the "字数" shown
            // on the title card; it costs no extra flash read because the
            // chapter scan already streams the entire book.
            unsigned char uc = (unsigned char)c;
            if ((uc & 0xC0u) != 0x80u && uc != ' ' && uc != '\t' &&
                !(pos < 3 && (uc == 0xEFu || uc == 0xBBu || uc == 0xBFu))) {
                ++out->char_count;
            }
            if (c == '\n' || c == '\r') {
                if (line_len > 0 &&
                    reader_chapter_match(line, line_len) &&
                    out->count < READER_MAX_CHAPTERS) {
                    reader_chapter_info_t *item = &out->items[out->count++];
                    item->offset = line_start;
                    copy_title(line, line_len, item->title, sizeof(item->title));
                }
                line_len = 0;
                line_start = pos + 1;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
        }
        absolute += (size_t)n;
        if ((size_t)n < sizeof(chunk)) {
            // Last line without a newline.
            if (line_len > 0 && reader_chapter_match(line, line_len) &&
                out->count < READER_MAX_CHAPTERS) {
                reader_chapter_info_t *item = &out->items[out->count++];
                item->offset = line_start;
                copy_title(line, line_len, item->title, sizeof(item->title));
            }
        }
    }
    if (out->count == 0) {
        out->items[0].offset = 0;
        snprintf(out->items[0].title, sizeof(out->items[0].title), "%s", "开篇");
        out->count = 1;
    }
    return ESP_OK;
}

esp_err_t reader_store_begin_upload(const char *name, char *out_id, size_t out_id_size)
{
    if (!name || !out_id || out_id_size < 8 || !s_spiffs_mounted) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_upload_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_upload_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (s_upload_fp) {
        xSemaphoreGive(s_upload_lock);
        return ESP_ERR_INVALID_STATE;  // another upload is in progress
    }

    char base[READER_BOOK_ID_MAX];
    sanitize_name(name, base, sizeof(base));
    snprintf(out_id, out_id_size, "%s", base);
    snprintf(s_upload_target, sizeof(s_upload_target), "%s", base);

    char path[READER_BOOK_ID_MAX + 16];
    tmp_path(path, sizeof(path));
    s_upload_fp = fopen(path, "wb");
    if (!s_upload_fp) {
        xSemaphoreGive(s_upload_lock);
        return ESP_FAIL;
    }
    xSemaphoreGive(s_upload_lock);
    return ESP_OK;
}

esp_err_t reader_store_write_chunk(const char *data, size_t len)
{
    if (!data || len == 0) return ESP_OK;
    if (!s_upload_lock || xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (!s_upload_fp || fwrite(data, 1, len, s_upload_fp) != len) {
        err = ESP_FAIL;
    }
    xSemaphoreGive(s_upload_lock);
    return err;
}

// Count unsupported code points by streaming the temporary file. This runs in
// the HTTP worker task, so it uses the plain C code-point table, never LVGL.
static uint32_t count_unsupported(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    char buf[512 + 4];
    size_t carry = 0;
    uint32_t total = 0;

    for (;;) {
        size_t n = fread(buf + carry, 1, 512, fp);
        size_t len = carry + n;
        if (len == 0) break;

        // Do not scan an incomplete UTF-8 sequence at the chunk boundary;
        // carry those bytes into the next chunk instead of miscounting them.
        size_t usable = len;
        size_t lead = len;
        while (lead > 0 && ((unsigned char)buf[lead - 1] & 0xC0) == 0x80) --lead;
        if (lead > 0) {
            unsigned char c = (unsigned char)buf[lead - 1];
            size_t need = (c < 0x80) ? 1 :
                          ((c & 0xE0) == 0xC0) ? 2 :
                          ((c & 0xF0) == 0xE0) ? 3 :
                          ((c & 0xF8) == 0xF0) ? 4 : 1;
            if (len - (lead - 1) < need) usable = lead - 1;
        } else {
            usable = 0;
        }

        if (usable > 0) {
            reader_charset_report_t report;
            if (reader_scan_charset(buf, usable, reader_font_supported_cb, NULL, &report) == 0) {
                total += report.unsupported;
            }
        }
        carry = len - usable;
        if (carry > 0 && carry <= 4) memmove(buf, buf + usable, carry);
        else carry = 0;
        if (n < 512) break;
    }
    fclose(fp);
    return total;
}

esp_err_t reader_store_finish_upload(const char *id, uint32_t *unsupported_out)
{
    if (!id || !s_upload_lock) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_upload_fp) {
        xSemaphoreGive(s_upload_lock);
        return ESP_ERR_INVALID_STATE;
    }
    fclose(s_upload_fp);
    s_upload_fp = NULL;

    char tmp[READER_BOOK_ID_MAX + 16];
    char dst[READER_BOOK_ID_MAX + 16];
    tmp_path(tmp, sizeof(tmp));
    book_path(id, dst, sizeof(dst));

    uint32_t unsupported = count_unsupported(tmp);
    // Replace any existing book with the same name atomically enough for SPIFFS.
    remove(dst);
    esp_err_t err = (rename(tmp, dst) == 0) ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存上传书籍失败: %s", dst);
        remove(tmp);
    } else {
        ESP_LOGI(TAG, "已保存 %s (%u 个未覆盖字符)", id, (unsigned)unsupported);
    }
    if (unsupported_out) *unsupported_out = unsupported;
    xSemaphoreGive(s_upload_lock);
    return err;
}

void reader_store_abort_upload(void)
{
    if (!s_upload_lock) return;
    if (xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (s_upload_fp) {
        fclose(s_upload_fp);
        s_upload_fp = NULL;
    }
    char tmp[READER_BOOK_ID_MAX + 16];
    tmp_path(tmp, sizeof(tmp));
    remove(tmp);
    xSemaphoreGive(s_upload_lock);
}

esp_err_t reader_store_delete(const char *id)
{
    if (!id || id[0] == '\0' || !s_spiffs_mounted) return ESP_ERR_INVALID_ARG;
    if (strncmp(id, "builtin:", 8) == 0) return ESP_ERR_NOT_SUPPORTED;
    char path[READER_BOOK_ID_MAX + 16];
    book_path(id, path, sizeof(path));
    return remove(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void progress_save_locked(void)
{
    if (!s_nvs_open) return;
    uint8_t buf[READER_PROGRESS_BLOB_SIZE];
    int n = reader_progress_serialize(&s_progress, buf, sizeof(buf));
    if (n <= 0) return;
    esp_err_t err = nvs_set_blob(s_nvs, NVS_PROGRESS_KEY, buf, (size_t)n);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "保存进度失败: %s", esp_err_to_name(err));
}

bool reader_store_has_progress(const char *id)
{
    if (!id) return false;
    if (!s_progress_lock ||
        xSemaphoreTake(s_progress_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    reader_progress_entry_t entry;
    bool found = reader_progress_find(&s_progress, reader_book_id(id), &entry) != 0;
    xSemaphoreGive(s_progress_lock);
    return found;
}

uint32_t reader_store_get_offset(const char *id, uint16_t *chapter_out)
{
    if (!id) return 0;
    if (chapter_out) *chapter_out = 0;
    if (!s_progress_lock || xSemaphoreTake(s_progress_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    reader_progress_entry_t entry;
    uint32_t offset = 0;
    if (reader_progress_find(&s_progress, reader_book_id(id), &entry)) {
        offset = entry.offset;
        if (chapter_out) *chapter_out = entry.chapter;
    }
    xSemaphoreGive(s_progress_lock);
    return offset;
}

void reader_store_set_offset(const char *id, uint32_t offset, uint16_t chapter)
{
    if (!id || !s_nvs_open || !s_progress_lock) return;
    if (xSemaphoreTake(s_progress_lock, pdMS_TO_TICKS(500)) != pdTRUE) return;

    reader_progress_entry_t entry = {
        .book_id = reader_book_id(id),
        .offset = offset,
        .chapter = chapter,
        .flags = 0,
    };
    reader_progress_set(&s_progress, &entry);
    progress_save_locked();
    xSemaphoreGive(s_progress_lock);
}

uint8_t reader_store_get_setting(const char *key, uint8_t fallback)
{
    if (!s_nvs_open || !key) return fallback;
    uint8_t value = fallback;
    esp_err_t err = nvs_get_u8(s_nvs, key, &value);
    if (err != ESP_OK) return fallback;
    return value;
}

void reader_store_set_setting(const char *key, uint8_t value)
{
    if (!s_nvs_open || !key) return;
    if (nvs_set_u8(s_nvs, key, value) == ESP_OK) {
        (void)nvs_commit(s_nvs);
    }
}

// ---------------------------------------------------------------------------
// Bookmarks — persisted next to reading progress, guarded by the same mutex
// because both are touched from the input task and from rendering.
// ---------------------------------------------------------------------------
static void bookmarks_load_locked(void)
{
    reader_bookmark_init(&s_bookmarks);
    if (!s_nvs_open) return;
    uint8_t buf[READER_BOOKMARK_BLOB_SIZE];
    size_t len = sizeof(buf);
    if (nvs_get_blob(s_nvs, NVS_BOOKMARK_KEY, buf, &len) == ESP_OK) {
        if (reader_bookmark_parse(buf, len, &s_bookmarks) != 0) {
            ESP_LOGW(TAG, "书签数据损坏，已重置");
        }
    }
}

static void bookmarks_save_locked(void)
{
    if (!s_nvs_open) return;
    uint8_t buf[READER_BOOKMARK_BLOB_SIZE];
    int n = reader_bookmark_serialize(&s_bookmarks, buf, sizeof(buf));
    if (n <= 0) return;
    esp_err_t err = nvs_set_blob(s_nvs, NVS_BOOKMARK_KEY, buf, (size_t)n);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "保存书签失败: %s", esp_err_to_name(err));
}

int reader_store_bookmark_list(uint32_t book_id, reader_bookmark_entry_t *out, int max)
{
    if (!out || max <= 0 || !s_progress_lock) return 0;
    int n = 0;
    if (xSemaphoreTake(s_progress_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        n = reader_bookmark_list(&s_bookmarks, book_id, out, max);
        xSemaphoreGive(s_progress_lock);
    }
    return n;
}

int reader_store_bookmark_toggle(uint32_t book_id, uint32_t offset, uint16_t chapter)
{
    if (!s_nvs_open || !s_progress_lock) return -1;
    if (xSemaphoreTake(s_progress_lock, pdMS_TO_TICKS(500)) != pdTRUE) return -1;

    int added;
    if (reader_bookmark_find(&s_bookmarks, book_id, offset) >= 0) {
        reader_bookmark_remove(&s_bookmarks, book_id, offset);
        added = 0;
    } else {
        reader_bookmark_entry_t e = {
            .book_id = book_id,
            .offset = offset,
            .chapter = chapter,
            .flags = 0,
        };
        added = reader_bookmark_add(&s_bookmarks, &e) > 0 ? 1 : 0;
    }
    bookmarks_save_locked();
    xSemaphoreGive(s_progress_lock);
    return added;
}

int reader_store_bookmark_remove(uint32_t book_id, uint32_t offset)
{
    if (!s_nvs_open || !s_progress_lock) return 0;
    if (xSemaphoreTake(s_progress_lock, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
    int removed = reader_bookmark_remove(&s_bookmarks, book_id, offset);
    if (removed) bookmarks_save_locked();
    xSemaphoreGive(s_progress_lock);
    return removed;
}

// Half-width units in a bookmark label: ten Hanzi, plus room for an ellipsis.
#define BOOKMARK_EXCERPT_UNITS 20

int reader_store_bookmark_excerpt(const char *id, uint32_t offset,
                                  char *out, size_t out_size)
{
    if (!id || !out || out_size == 0) return -1;
    char window[256];
    int n = reader_store_read(id, (size_t)offset, window, sizeof(window));
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    return reader_excerpt(window, (size_t)n, BOOKMARK_EXCERPT_UNITS,
                          out, out_size);
}

esp_err_t reader_store_delete_book(const char *id)
{
    if (!id || id[0] == '\0') return ESP_ERR_INVALID_ARG;
    // The guide documents the device itself: it is the only built-in that
    // cannot be removed, so there is always something to read on a blank unit.
    if (strcmp(id, READER_BUILTIN_GUIDE_ID) == 0) return ESP_ERR_NOT_SUPPORTED;

    esp_err_t err;
    if (strncmp(id, "builtin:", 8) == 0) {
        // Built-in samples are compiled into Flash, so "delete" hides them from
        // the shelf. Persist the bit so the choice survives a reboot; a full
        // 0x0 reflash clears NVS and brings the samples back.
        int idx = -1;
        for (int i = 1; i < g_builtin_book_count && i < 8; ++i) {
            if (strcmp(id, g_builtin_books[i].id) == 0) { idx = i; break; }
        }
        if (idx < 0) return ESP_ERR_NOT_FOUND;
        s_hidden_builtins |= (uint8_t)(1u << idx);
        if (s_nvs_open) {
            if (nvs_set_u8(s_nvs, NVS_HIDDEN_KEY, s_hidden_builtins) == ESP_OK) {
                (void)nvs_commit(s_nvs);
            }
        }
        err = ESP_OK;
    } else {
        err = reader_store_delete(id);
    }
    if (err != ESP_OK) return err;

    // A later upload with the same name (or a re-flashed sample) must not
    // resume at this book's offsets or inherit its bookmarks. Both blobs live
    // under the same mutex.
    if (s_progress_lock &&
        xSemaphoreTake(s_progress_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        uint32_t bid = reader_book_id(id);
        if (reader_progress_remove(&s_progress, bid)) progress_save_locked();
        if (reader_bookmark_remove_book(&s_bookmarks, bid)) bookmarks_save_locked();
        xSemaphoreGive(s_progress_lock);
    }
    return ESP_OK;
}
