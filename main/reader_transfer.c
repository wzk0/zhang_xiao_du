// reader_transfer.c — see reader_transfer.h.
#include "reader_transfer.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"   // IP_EVENT / IP_EVENT_AP_STAIPASSIGNED
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "reader_store.h"
#include "reader_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Embedded at build time via EMBED_TXTFILES; the Chinese web page is NOT part of
// the on-device font inventory, so it lives outside main/ as an asset.
extern const uint8_t transfer_html_start[] asm("_binary_transfer_html_start");
extern const uint8_t transfer_html_end[] asm("_binary_transfer_html_end");

static const char *TAG = "reader_transfer";

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_wifi_initialized;
static bool s_wifi_started;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_running;
static SemaphoreHandle_t s_status_lock;
static reader_transfer_status_t s_status;

static void status_lock_init(void)
{
    if (!s_status_lock) s_status_lock = xSemaphoreCreateMutex();
}

static void set_upload_status(reader_upload_state_t state, const char *detail,
                              uint32_t unsupported)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.upload = state;
    s_status.unsupported = unsupported;
    snprintf(s_status.upload_detail, sizeof(s_status.upload_detail), "%s", detail);
    xSemaphoreGive(s_status_lock);
}

void reader_transfer_get_status(reader_transfer_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_status_lock || xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    *out = s_status;
    xSemaphoreGive(s_status_lock);
}

// ---------------------------------------------------------------------------
// Minimal DNS responder so a phone's captive-portal probes land on our page.
// ---------------------------------------------------------------------------
static void dns_answer(int sock, const char *query, ssize_t qlen,
                       const struct sockaddr_in *from, socklen_t fromlen)
{
    if (qlen < 12 || qlen > 512) return;
    uint8_t resp[512];
    memcpy(resp, query, (size_t)qlen);
    resp[2] = 0x81; resp[3] = 0x80;  // QR=1, RD/RA set, rcode 0
    resp[6] = 0x00; resp[7] = 0x01;  // ANCOUNT = 1

    size_t qend = 12;
    while (qend < (size_t)qlen && resp[qend] != 0) {
        uint8_t l = resp[qend];
        if (l & 0xC0) return;  // unexpected compression pointer in a query
        qend += 1 + (size_t)l;
        if (qend >= (size_t)qlen) return;
    }
    qend += 1 + 4;  // terminating NUL + QTYPE + QCLASS
    if (qend + 16 > sizeof(resp)) return;

    size_t p = qend;
    resp[p++] = 0xC0; resp[p++] = 0x0C;              // name pointer to QNAME
    resp[p++] = 0x00; resp[p++] = 0x01;              // TYPE A
    resp[p++] = 0x00; resp[p++] = 0x01;              // CLASS IN
    resp[p++] = 0x00; resp[p++] = 0x00;
    resp[p++] = 0x00; resp[p++] = 0x3C;              // TTL 60 s
    resp[p++] = 0x00; resp[p++] = 0x04;              // RDLENGTH 4
    resp[p++] = 192; resp[p++] = 168;
    resp[p++] = 4;   resp[p++] = 1;
    (void)sendto(sock, resp, p, 0, (const struct sockaddr *)from, fromlen);
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[512];
    while (s_dns_running) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n > 0) dns_answer(sock, buf, n, &from, fromlen);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    if (s_dns_task) return;
    s_dns_running = true;
    if (xTaskCreate(dns_task, "reader_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
        s_dns_running = false;
        ESP_LOGW(TAG, "DNS 任务创建失败，页面仍可直接访问 " READER_AP_URL);
    }
}

static void dns_stop(void)
{
    s_dns_running = false;
    for (int i = 0; i < 20 && s_dns_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_task) {
        vTaskDelete(s_dns_task);  // fallback; the 400 ms recv timeout normally exits first
        s_dns_task = NULL;
    }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static bool url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (const char *p = src; *p; ++p) {
        if (o + 1 >= dst_size) return false;
        if (*p == '+') {
            dst[o++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            char *end;
            long value = strtol(hex, &end, 16);
            if (end != hex + 2 || value <= 0 || value > 255) return false;
            dst[o++] = (char)value;
            p += 2;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
    return true;
}

static int query_value(httpd_req_t *req, const char *key, char *out, size_t out_size)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) return -1;
    char *query = malloc(qlen);
    if (!query) return -1;
    if (httpd_req_get_url_query_str(req, query, qlen) != ESP_OK) {
        free(query);
        return -1;
    }
    char *raw = malloc(qlen);
    if (!raw) {
        free(query);
        return -1;
    }
    esp_err_t err = httpd_query_key_value(query, key, raw, qlen);
    free(query);
    if (err == ESP_OK) {
        err = url_decode(raw, out, out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    free(raw);
    return err == ESP_OK ? 0 : -1;
}

static size_t json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < out_size; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

static esp_err_t send_json(httpd_req_t *req, int status, const char *body)
{
    httpd_resp_set_status(req, status == 200 ? "200 OK" : "400 Bad Request");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, (ssize_t)strlen(body));
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)transfer_html_start,
                           (ssize_t)(transfer_html_end - transfer_html_start));
}

static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t handle_space(httpd_req_t *req)
{
    size_t total = 0, used = 0, free_b = 0;
    if (reader_store_space(&total, &used, &free_b) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    char body[160];
    snprintf(body, sizeof(body),
             "{\"total\":%u,\"used\":%u,\"free\":%u}",
             (unsigned)total, (unsigned)used, (unsigned)free_b);
    return send_json(req, 200, body);
}

static esp_err_t handle_list(httpd_req_t *req)
{
    // The HTTP task has a 6 KiB stack. Keep the shelf snapshot on the
    // heap and send one JSON entry at a time.
    reader_book_info_t *books = malloc(READER_MAX_BOOKS * sizeof(*books));
    if (!books) return httpd_resp_send_500(req);
    int n = reader_store_list(books, READER_MAX_BOOKS);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    for (int i = 0; i < n && err == ESP_OK; ++i) {
        if (books[i].builtin) continue;
        char name[READER_BOOK_TITLE_MAX * 6 + 1];
        char id[READER_BOOK_ID_MAX * 6 + 1];
        char entry[sizeof(name) + sizeof(id) + 48];
        json_escape(books[i].title, name, sizeof(name));
        json_escape(books[i].id, id, sizeof(id));
        snprintf(entry, sizeof(entry), "%s{\"id\":\"%s\",\"name\":\"%s\",\"size\":%u}",
                 first ? "" : ",", id, name, (unsigned)books[i].size);
        err = httpd_resp_sendstr_chunk(req, entry);
        first = false;
    }
    free(books);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "]", 1);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t handle_delete(httpd_req_t *req)
{
    char id[READER_BOOK_ID_MAX];
    if (query_value(req, "id", id, sizeof(id)) != 0 || strncmp(id, "builtin:", 8) == 0) {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"书籍编号无效\"}");
    }
    if (reader_store_delete_book(id) != ESP_OK) {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"删除失败\"}");
    }
    return send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t upload_error(httpd_req_t *req, const char *detail, const char *body)
{
    set_upload_status(READER_UPLOAD_FAILED, detail, 0);
    return send_json(req, 400, body);
}

static esp_err_t handle_upload(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > READER_MAX_UPLOAD_BYTES) {
        return upload_error(req, "文件大小超限",
                            "{\"ok\":false,\"error\":\"zdx 文件大小必须在 1 B 到 512 KB 之间\"}");
    }
    size_t total = 0, used = 0, free_b = 0;
    if (reader_store_space(&total, &used, &free_b) == ESP_OK &&
        free_b < req->content_len + 8192) {
        return upload_error(req, "剩余空间不足",
                            "{\"ok\":false,\"error\":\"剩余空间不足，请先删除部分书籍\"}");
    }

    char name[256];
    if (query_value(req, "name", name, sizeof(name)) != 0 || name[0] == '\0') {
        return upload_error(req, "缺少文件名", "{\"ok\":false,\"error\":\"缺少文件名\"}");
    }
    char id[READER_BOOK_ID_MAX];
    esp_err_t start = reader_store_begin_upload(name, id, sizeof(id));
    if (start == ESP_ERR_INVALID_SIZE) {
        return upload_error(req, "书架已满",
                            "{\"ok\":false,\"error\":\"书架已满，请先删除部分书籍\"}");
    }
    if (start != ESP_OK) {
        return upload_error(req, "存储不可用",
                            "{\"ok\":false,\"error\":\"已有上传任务或存储不可用\"}");
    }

    char title[READER_BOOK_ID_MAX];
    // Storage normalises every accepted ID to a .zdx filename.
    size_t title_len = strlen(id) - sizeof(".zdx") + 1;
    if (reader_excerpt(id, title_len, 32, title, sizeof(title)) <= 0) {
        snprintf(title, sizeof(title), "%s", "未命名");
    }
    set_upload_status(READER_UPLOAD_RECEIVING, title, 0);

    char buf[1024];
    size_t remaining = req->content_len;
    size_t received = 0;
    bool ok = true;
    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) {  // timeout, disconnect or error
            ok = false;
            break;
        }
        if (reader_store_write_chunk(buf, (size_t)r) != ESP_OK) {
            ok = false;
            break;
        }
        remaining -= (size_t)r;
        received += (size_t)r;
    }
    if (!ok) {
        reader_store_abort_upload();
        return upload_error(req, "传输中断或写入失败",
                            "{\"ok\":false,\"error\":\"传输中断或写入失败\"}");
    }

    uint32_t unsupported = 0;
    if (reader_store_finish_upload(id, &unsupported) != ESP_OK) {
        return upload_error(req, "保存失败", "{\"ok\":false,\"error\":\"保存失败\"}");
    }

    char escaped[READER_BOOK_ID_MAX * 3];
    json_escape(id, escaped, sizeof(escaped));
    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"name\":\"%s\",\"size\":%u,\"unsupported\":%u}",
             escaped, (unsigned)received, (unsigned)unsupported);

    set_upload_status(READER_UPLOAD_SAVED, title, unsupported);
    ESP_LOGI(TAG, "上传完成 %s (%u B, 未覆盖字符 %u)", id, (unsigned)received,
             (unsigned)unsupported);
    return send_json(req, 200, body);
}

static const httpd_uri_t URI_HANDLERS[] = {
    { .uri = "/",       .method = HTTP_GET,  .handler = handle_root,     .user_ctx = NULL },
    { .uri = "/space",  .method = HTTP_GET,  .handler = handle_space,    .user_ctx = NULL },
    { .uri = "/list",   .method = HTTP_GET,  .handler = handle_list,     .user_ctx = NULL },
    { .uri = "/delete", .method = HTTP_POST,  .handler = handle_delete,   .user_ctx = NULL },
    { .uri = "/upload", .method = HTTP_POST, .handler = handle_upload,   .user_ctx = NULL },
    { .uri = "/*",      .method = HTTP_GET,  .handler = handle_redirect, .user_ctx = NULL },
};

static esp_err_t http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 6144;      // see the SoftAP resource-budget reference
    config.max_uri_handlers = 10;
    config.backlog_conn = 2;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < sizeof(URI_HANDLERS) / sizeof(URI_HANDLERS[0]); ++i) {
        err = httpd_register_uri_handler(s_httpd, &URI_HANDLERS[i]);
        if (err != ESP_OK) {
            httpd_stop(s_httpd);
            s_httpd = NULL;
            return err;
        }
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Wi-Fi AP lifecycle
// ---------------------------------------------------------------------------
static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_status.client_count++;
            xSemaphoreGive(s_status_lock);
        }
        ESP_LOGI(TAG, "设备已接入热点");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_status.client_count > 0) s_status.client_count--;
            s_status.client_ready = false;
            xSemaphoreGive(s_status_lock);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_status.client_ready = true;
            xSemaphoreGive(s_status_lock);
        }
        ESP_LOGI(TAG, "DHCP 已分配地址，免费堆 %u B", (unsigned)esp_get_free_heap_size());
    }
}

static void rollback(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    dns_stop();
    if (s_wifi_handler) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    memset(&s_status, 0, sizeof(s_status));
}

esp_err_t reader_transfer_start(void)
{
    status_lock_init();
    if (!s_status_lock) return ESP_ERR_NO_MEM;
    if (s_status.active) return ESP_OK;

    ESP_LOGI(TAG, "启动热点前免费堆 %u B", (unsigned)esp_get_free_heap_size());
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_events, NULL, &s_wifi_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_events, NULL, &s_ip_handler);
    if (err != ESP_OK) goto fail;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto fail;
    s_wifi_initialized = true;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) goto fail;

    wifi_config_t ap = {
        .ap = {
            .ssid = READER_AP_SSID,
            .ssid_len = (uint8_t)strlen(READER_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .max_connection = 1,  // one phone, conservative heap/socket budget
        },
    };
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;

    dns_start();
    err = http_start();
    if (err != ESP_OK) goto fail;

    s_status.active = true;
    ESP_LOGI(TAG, "热点 %s 已开启，页面 %s，免费堆 %u B",
             READER_AP_SSID, READER_AP_URL, (unsigned)esp_get_free_heap_size());
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "热点启动失败: %s", esp_err_to_name(err));
    rollback();
    return err;
}

void reader_transfer_stop(void)
{
    if (!s_status.active && !s_httpd && !s_ap_netif) return;
    ESP_LOGI(TAG, "关闭热点，免费堆 %u B", (unsigned)esp_get_free_heap_size());
    rollback();
    ESP_LOGI(TAG, "热点已关闭，免费堆 %u B", (unsigned)esp_get_free_heap_size());
}
