// reader_store.c — see reader_store.h.
#include "reader_store.h"

#include "builtin_books.h"
#include "reader_bookmark.h"
#include "reader_fonts.h"
#include "reader_progress.h"
#include "reader_text.h"
#include "reader_zdx.h"

#include "dirent.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sys/stat.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
static SemaphoreHandle_t s_upload_lock;

static void lock_init(void)
{
    if (!s_progress_lock) s_progress_lock = xSemaphoreCreateMutex();
    if (!s_upload_lock) s_upload_lock = xSemaphoreCreateMutex();
}

#define BOOK_EXT ".zdx"

static void book_path(const char *id, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", BOOKS_BASE, id);
}

// One open decoder at a time. The reader is single-threaded (all access from
// the LVGL/input task), so a single cache avoids repeated fopen per page.
static reader_zdx_t s_zdx;
static char s_zdx_id[READER_BOOK_ID_MAX];
static bool s_zdx_open;
// Upload verification inflates every chunk on the 6 KiB HTTP task, so its
// ~18 KiB decoder must live in static storage, never on that task's stack.
static reader_zdx_t s_verify_zdx;

static void zdx_cache_close(void)
{
    if (s_zdx_open) {
        FILE *fp = s_zdx.fp;  // reader_zdx_close() clears z->fp
        reader_zdx_close(&s_zdx);
        if (fp) fclose(fp);
    }
    s_zdx_open = false;
    s_zdx_id[0] = '\0';
}

// Open the zdx decoder for a book (built-in or uploaded), reusing the cache
// when it is already the current book. Built-ins decode from a flash-backed
// memory image; uploads from a file. Returns a reader or NULL.
static reader_zdx_t *zdx_for(const char *id)
{
    if (!id) return NULL;
    if (s_zdx_open && strcmp(s_zdx_id, id) == 0) return &s_zdx;
    zdx_cache_close();

    int rc;
    if (strncmp(id, "builtin:", 8) == 0) {
        const reader_builtin_book_t *book = NULL;
        for (int i = 0; i < g_builtin_book_count; ++i) {
            if (strcmp(id, g_builtin_books[i].id) == 0) {
                book = &g_builtin_books[i];
                break;
            }
        }
        if (!book) return NULL;
        rc = reader_builtin_open(book, &s_zdx);
    } else {
        char path[READER_BOOK_ID_MAX + 16];
        book_path(id, path, sizeof(path));
        FILE *fp = fopen(path, "rb");
        if (!fp) return NULL;
        rc = reader_zdx_open(&s_zdx, fp);
        if (rc != 0) fclose(fp);
    }
    if (rc != 0) {
        memset(&s_zdx, 0, sizeof(s_zdx));
        return NULL;
    }
    strncpy(s_zdx_id, id, sizeof(s_zdx_id) - 1);
    s_zdx_id[sizeof(s_zdx_id) - 1] = '\0';
    s_zdx_open = true;
    return &s_zdx;
}

static void tmp_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", BOOKS_BASE, READER_UPLOAD_TMP_NAME);
}

// SPIFFS counts the leading slash and NUL in its object-name limit.
#define BOOK_ID_LIMIT (CONFIG_SPIFFS_OBJ_NAME_LEN - 2)
#define BOOK_STEM_LIMIT (BOOK_ID_LIMIT - (sizeof(BOOK_EXT) - 1))
#define REPLACE_BACKUP "/books/.replace.old"
#define REPLACE_ID "/books/.replace.id"

static bool valid_uploaded_id(const char *id)
{
    if (!id || id[0] == '.') return false;
    size_t len = strlen(id);
    return len > sizeof(BOOK_EXT) - 1 && len <= BOOK_ID_LIMIT &&
           !strchr(id, '/') && !strchr(id, '\\') &&
           strcmp(id + len - (sizeof(BOOK_EXT) - 1), BOOK_EXT) == 0;
}

// Hidden files are disposable. The full ID in the header prevents a filename
// hash collision from loading another book's cache.
typedef struct {
    uint32_t version;
    uint32_t key1;
    uint32_t key2;
    uint32_t size;
    uint32_t crc;
    char id[READER_BOOK_ID_MAX];
} book_cache_header_t;

static void cache_path(const char *id, char kind, char *out, size_t size)
{
    snprintf(out, size, "%s/.%c%08lx", BOOKS_BASE, kind, (unsigned long)reader_book_id(id));
}

static size_t cache_read(const char *id, char kind, uint32_t key1, uint32_t key2,
                         void *data, size_t capacity)
{
    char path[32];
    cache_path(id, kind, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    book_cache_header_t h;
    bool valid = fread(&h, sizeof(h), 1, fp) == 1 && h.version == 1 &&
                 h.key1 == key1 && h.key2 == key2 && h.size > 0 && h.size <= capacity &&
                 strncmp(h.id, id, sizeof(h.id)) == 0 &&
                 fread(data, 1, h.size, fp) == h.size &&
                 reader_crc32(data, h.size) == h.crc &&
                 fgetc(fp) == EOF && !ferror(fp);
    fclose(fp);
    return valid ? h.size : 0;
}

static void cache_write(const char *id, char kind, uint32_t key1, uint32_t key2,
                        const void *data, size_t size)
{
    char path[32];
    cache_path(id, kind, path, sizeof(path));
    book_cache_header_t h = {.version = 1, .key1 = key1, .key2 = key2,
                             .size = size, .crc = reader_crc32(data, size)};
    snprintf(h.id, sizeof(h.id), "%s", id);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    bool written = fwrite(&h, sizeof(h), 1, fp) == 1 && fwrite(data, 1, size, fp) == size;
    if (fclose(fp) != 0) written = false;
    if (!written) remove(path);
}

static bool cache_invalidate(const char *id, char kind)
{
    char path[32];
    cache_path(id, kind, path, sizeof(path));
    return remove(path) == 0 || errno == ENOENT;
}

int reader_store_history_load(const char *id, uint32_t offset, uint32_t layout,
                              uint32_t history[READER_PAGE_HISTORY_MAX])
{
    if (!s_spiffs_mounted || !valid_uploaded_id(id)) return 0;
    size_t n = cache_read(id, 'h', offset, layout, history,
                          READER_PAGE_HISTORY_MAX * sizeof(*history));
    if (n % sizeof(*history)) return 0;
    int count = (int)(n / sizeof(*history));
    for (int i = 0; i < count; ++i) {
        if (history[i] >= offset || (i > 0 && history[i - 1] >= history[i])) return 0;
    }
    return count;
}

void reader_store_history_save(const char *id, uint32_t offset, uint32_t layout,
                               const uint32_t *history, int count)
{
    if (!s_spiffs_mounted || !valid_uploaded_id(id) ||
        !history || count <= 0 || count > READER_PAGE_HISTORY_MAX) return;
    cache_write(id, 'h', offset, layout, history, (size_t)count * sizeof(*history));
}

static void sanitize_name(const char *in, char *out)
{
    size_t len = strlen(in);
    if (len >= 4 && (!strcmp(in + len - 4, ".txt") || !strcmp(in + len - 4, BOOK_EXT))) {
        len -= 4;
    }
    size_t used = 0;
    size_t short_prefix = 0;
    bool truncated = false;
    for (size_t pos = 0; pos < len;) {
        size_t width = 0;
        uint32_t cp = reader_utf8_decode(in + pos, len - pos, &width);
        pos += width;
        if (cp == READER_INVALID_CP || cp < 0x20 || cp == 0x7F ||
            (cp < 128 && strchr("/\\:\"'<>&", (int)cp))) continue;
        if (cp == '.' && (used == 0 || out[used - 1] == '.')) continue;
        if (used + width > BOOK_STEM_LIMIT || used + width + sizeof(BOOK_EXT) > READER_BOOK_ID_MAX) {
            truncated = true;
            break;
        }
        memcpy(out + used, in + pos - width, width);
        used += width;
        if (used + 9 <= BOOK_STEM_LIMIT && used + 9 + sizeof(BOOK_EXT) <= READER_BOOK_ID_MAX) {
            short_prefix = used;
        }
    }
    if (truncated) {
        used = short_prefix + (size_t)snprintf(out + short_prefix,
            READER_BOOK_ID_MAX - short_prefix, "~%08lx", (unsigned long)reader_book_id(in));
    } else if (used == 0) {
        memcpy(out, "book", 4);
        used = 4;
    }
    memcpy(out + used, BOOK_EXT, sizeof(BOOK_EXT));
}

static bool books_partition_blank(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, BOOKS_LABEL);
    if (!part) return false;
    uint8_t block[512];
    for (size_t offset = 0; offset < part->size; offset += sizeof(block)) {
        size_t n = part->size - offset;
        if (n > sizeof(block)) n = sizeof(block);
        if (esp_partition_read(part, offset, block, n) != ESP_OK) return false;
        for (size_t i = 0; i < n; ++i) {
            if (block[i] != 0xff) return false;
        }
    }
    return true;
}

static bool recover_replacement(void)
{
    struct stat st;
    if (stat(REPLACE_BACKUP, &st) != 0) {
        if (errno != ENOENT) return false;
        remove(REPLACE_ID);
        return true;
    }

    char id[READER_BOOK_ID_MAX] = {0};
    FILE *journal = fopen(REPLACE_ID, "rb");
    if (!journal) return false;
    size_t len = fread(id, 1, sizeof(id) - 1, journal);
    bool complete = !ferror(journal) && feof(journal) && strlen(id) == len;
    fclose(journal);
    if (!complete || !valid_uploaded_id(id)) return false;

    char dst[READER_BOOK_ID_MAX + 16];
    book_path(id, dst, sizeof(dst));
    if (stat(dst, &st) == 0) {
        if (remove(REPLACE_BACKUP) != 0) return false;
    } else if (errno != ENOENT || rename(REPLACE_BACKUP, dst) != 0) {
        return false;
    }
    remove(REPLACE_ID);
    return true;
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
        .format_if_mount_failed = false,
    };
    err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_FAIL && books_partition_blank()) {
        conf.format_if_mount_failed = true;
        err = esp_vfs_spiffs_register(&conf);
    }
    s_spiffs_mounted = err == ESP_OK;
    if (!s_spiffs_mounted) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(err));
    } else if (recover_replacement()) {
        char stale[READER_BOOK_ID_MAX + 16];
        tmp_path(stale, sizeof(stale));
        remove(stale);
    } else {
        ESP_LOGE(TAG, "书籍替换恢复失败");
    }

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
        const reader_builtin_book_t *book = &g_builtin_books[i];
        reader_book_info_t *b = &out[count++];
        snprintf(b->id, sizeof(b->id), "%s", book->id);
        snprintf(b->title, sizeof(b->title), "%s", book->title);
        // Shelf shows the compressed footprint; progress/subtitle use the
        // uncompressed length (both from the generated zdx metadata).
        b->size = (uint32_t)book->zdx_len;
        b->orig_size = book->orig_size;
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
        if (nlen < 4 || strcmp(name + nlen - 4, BOOK_EXT) != 0) continue;

        char path[READER_BOOK_ID_MAX + 16];
        book_path(name, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) != 0) continue;

        // A name that does not fit the id field could never be reopened for
        // reading, so skip it instead of silently truncating the id.
        if (nlen >= sizeof(out[count].id)) continue;

        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        // Header-only probe: no ~18 KiB decoder on this task's small stack.
        uint32_t orig_size = 0, text_chars = 0;
        if (reader_zdx_probe(fp, &orig_size, &text_chars) != 0) {
            // Unreadable/legacy file: skip rather than offer a broken shelf row.
            fclose(fp);
            continue;
        }
        reader_book_info_t *b = &out[count++];
        snprintf(b->id, sizeof(b->id), "%.*s",
                 (int)(sizeof(b->id) - 1), name);
        snprintf(b->title, sizeof(b->title), "%.*s",
                 (int)(nlen - 4), name);  // display without .zdx
        b->size = (uint32_t)st.st_size;
        b->orig_size = orig_size;
        b->builtin = false;
        fclose(fp);
    }
    closedir(dir);
    return count;
}

int reader_store_read(const char *id, size_t offset, char *buf, size_t len)
{
    if (!id || !buf || len == 0) return 0;

    // Built-in books are zdx images in flash; uploads are zdx files. Both go
    // through the same decoder so logical byte offsets stay identical.
    reader_zdx_t *z = zdx_for(id);
    if (!z) return 0;
    return (int)reader_zdx_read(z, offset, buf, len);
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

static void index_add_line(reader_book_index_t *out, const char *line, size_t len,
                           size_t offset)
{
    if (!len || out->count >= READER_MAX_CHAPTERS || !reader_chapter_match(line, len)) return;
    reader_chapter_info_t *item = &out->items[out->count++];
    item->offset = offset;
    copy_title(line, len, item->title, sizeof(item->title));
}

esp_err_t reader_store_build_index(const char *id, reader_book_index_t *out,
                                   reader_index_progress_fn progress, void *ctx)
{
    if (!id || !out) return ESP_ERR_INVALID_ARG;
    reader_zdx_t *z = zdx_for(id);
    if (!z) return ESP_ERR_NOT_FOUND;
    uint32_t total = z->original_size;
    uint32_t file_size = (uint32_t)z->file_size;
    bool cacheable = s_spiffs_mounted && valid_uploaded_id(id);
    if (cacheable && cache_read(id, 'i', total, file_size, out, sizeof(*out)) == sizeof(*out) &&
        out->count > 0 && out->count <= READER_MAX_CHAPTERS) return ESP_OK;

    memset(out, 0, sizeof(*out));
    static char chunk[1024];
    char line[192];
    size_t line_len = 0;
    size_t line_start = 0;
    size_t absolute = 0;
    if (progress) progress(0, total, ctx);

    while (absolute < total) {
        size_t chunk_len = reader_zdx_read(z, absolute, chunk, sizeof(chunk));
        if (chunk_len == 0) return ESP_FAIL;
        for (size_t i = 0; i < chunk_len; ++i) {
            unsigned char c = (unsigned char)chunk[i];
            size_t pos = absolute + i;
            // Count UTF-8 leading bytes, excluding ASCII whitespace and a BOM.
            if ((c & 0xC0u) != 0x80u && c != ' ' && (c < '\t' || c > '\r') &&
                !(pos < 3 && (c == 0xEFu || c == 0xBBu || c == 0xBFu))) {
                ++out->char_count;
            }
            if (out->count >= READER_MAX_CHAPTERS) continue;
            if (c == '\n' || c == '\r') {
                index_add_line(out, line, line_len, line_start);
                line_len = 0;
                line_start = pos + 1;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = (char)c;
            }
        }
        absolute += chunk_len;
        if (progress) progress(absolute, total, ctx);
        if (absolute % (32u * 1024u) == 0) vTaskDelay(1);
    }
    index_add_line(out, line, line_len, line_start);
    if (out->count == 0) {
        snprintf(out->items[0].title, sizeof(out->items[0].title), "%s", "开篇");
        out->count = 1;
    }
    if (cacheable) cache_write(id, 'i', total, file_size, out, sizeof(*out));
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
    sanitize_name(name, base);
    if (strlen(base) >= out_id_size || !recover_replacement()) {
        xSemaphoreGive(s_upload_lock);
        return ESP_FAIL;
    }
    char dst[READER_BOOK_ID_MAX + 16];
    book_path(base, dst, sizeof(dst));
    struct stat st;
    if (stat(dst, &st) != 0) {
        if (errno != ENOENT) {
            xSemaphoreGive(s_upload_lock);
            return ESP_FAIL;
        }
        reader_book_info_t *books = malloc(READER_MAX_BOOKS * sizeof(*books));
        if (!books) {
            xSemaphoreGive(s_upload_lock);
            return ESP_ERR_NO_MEM;
        }
        int count = reader_store_list(books, READER_MAX_BOOKS);
        free(books);
        if (count == READER_MAX_BOOKS) {
            xSemaphoreGive(s_upload_lock);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    snprintf(out_id, out_id_size, "%s", base);

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
    if (!s_upload_fp) {
        err = ESP_ERR_INVALID_STATE;
    } else if (fwrite(data, 1, len, s_upload_fp) != len) {
        err = errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    xSemaphoreGive(s_upload_lock);
    return err;
}

// Verify a freshly uploaded zdx and count unsupported code points by streaming
// its original text in one pass. The decoder validates each chunk as it is read.
// The HTTP task uses a dedicated static decoder.
static int verify_and_count(const char *path, reader_charset_report_t *coverage_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    reader_zdx_t *z = &s_verify_zdx;
    // A noncanonical image can contain an empty final chunk. Verify any
    // chunks that the original-length walker would otherwise never visit.
    if (reader_zdx_open(z, fp) != 0 ||
        ((uint64_t)(z->chunk_count - 1) * z->chunk_size >= z->original_size &&
         reader_zdx_verify(z) != 0)) {
        reader_zdx_close(z);
        fclose(fp);
        return -1;
    }

    char buf[512 + 4];
    size_t carry = 0;
    reader_charset_report_t total = {0};
    size_t offset = 0;
    while (offset < z->original_size) {
        size_t n = reader_zdx_read(z, offset, buf + carry, 512);
        if (n == 0) {
            reader_zdx_close(z);
            fclose(fp);
            return -1;
        }
        size_t len = carry + n;
        offset += n;
        if (offset % (32u * 1024u) == 0) vTaskDelay(1);

        // Do not scan an incomplete UTF-8 sequence at the window boundary;
        // carry those bytes into the next window instead of miscounting them.
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
                reader_charset_report_merge(&total, &report);
            }
        }
        carry = len - usable;
        if (carry > 0 && carry <= 4) memmove(buf, buf + usable, carry);
        else carry = 0;
    }
    // Any leftover carried bytes are an incomplete UTF-8 tail; ignore them.
    reader_zdx_close(z);
    fclose(fp);
    if (coverage_out) *coverage_out = total;
    return 0;
}

static bool commit_upload(const char *tmp, const char *dst, const char *id)
{
    struct stat st;
    if (stat(dst, &st) != 0) {
        return errno == ENOENT && rename(tmp, dst) == 0;
    }

    FILE *journal = fopen(REPLACE_ID, "wb");
    if (!journal) return false;
    size_t len = strlen(id);
    bool written = fwrite(id, 1, len, journal) == len;
    if (fclose(journal) != 0) written = false;
    if (!written || rename(dst, REPLACE_BACKUP) != 0) return false;

    // Recovery restores the old file if the final rename failed or power was
    // lost before it. Once the destination exists, the replacement is committed.
    bool committed = rename(tmp, dst) == 0;
    if (!recover_replacement()) ESP_LOGE(TAG, "书籍替换恢复失败");
    return committed;
}

esp_err_t reader_store_finish_upload(const char *id, reader_charset_report_t *coverage_out)
{
    if (!valid_uploaded_id(id) || !s_upload_lock) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_upload_fp) {
        xSemaphoreGive(s_upload_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int close_result = fclose(s_upload_fp);
    int close_error = errno;
    s_upload_fp = NULL;

    char tmp[READER_BOOK_ID_MAX + 16];
    char dst[READER_BOOK_ID_MAX + 16];
    tmp_path(tmp, sizeof(tmp));
    book_path(id, dst, sizeof(dst));

    reader_charset_report_t coverage = {0};
    if (close_result != 0) {
        remove(tmp);
        xSemaphoreGive(s_upload_lock);
        return close_error == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (verify_and_count(tmp, &coverage) != 0) {
        ESP_LOGW(TAG, "上传文件不是有效的 zdx 或已损坏: %s", tmp);
        remove(tmp);
        xSemaphoreGive(s_upload_lock);
        return ESP_ERR_INVALID_ARG;
    }
    zdx_cache_close();  // invalidate any decoder pointing at the old file
    esp_err_t err = cache_invalidate(id, 'i') && cache_invalidate(id, 'h') &&
                    commit_upload(tmp, dst, id) ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存上传书籍失败: %s", dst);
        remove(tmp);
    } else {
        ESP_LOGI(TAG, "已保存 %s (%u 个未覆盖字符)", id, (unsigned)coverage.unsupported);
    }
    if (coverage_out) *coverage_out = coverage;
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
    zdx_cache_close();
    char tmp[READER_BOOK_ID_MAX + 16];
    tmp_path(tmp, sizeof(tmp));
    remove(tmp);
    xSemaphoreGive(s_upload_lock);
}

esp_err_t reader_store_delete(const char *id)
{
    if (!valid_uploaded_id(id) || !s_spiffs_mounted) return ESP_ERR_INVALID_ARG;
    if (s_zdx_open && strcmp(s_zdx_id, id) == 0) zdx_cache_close();
    char path[READER_BOOK_ID_MAX + 16];
    book_path(id, path, sizeof(path));
    if (remove(path) != 0) return ESP_ERR_NOT_FOUND;
    (void)cache_invalidate(id, 'i');
    (void)cache_invalidate(id, 'h');
    return ESP_OK;
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
