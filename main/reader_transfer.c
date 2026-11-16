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

#include <stdarg.h>
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

static void set_message(const char *fmt, ...)
{
    if (!s_status_lock) return;
    char line[sizeof(s_status.last_message)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(s_status.last_message, sizeof(s_status.last_message), "%s", line);
        xSemaphoreGive(s_status_lock);
    }
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
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < dst_size; ++p) {
        if (*p == '+') {
            dst[o++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
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
    char raw[192];
    esp_err_t err = httpd_query_key_value(query, key, raw, sizeof(raw));
    free(query);
    if (err != ESP_OK) return -1;
    url_decode(raw, out, out_size);
    return 0;
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

static int count_uploaded(void)
{
    reader_book_info_t books[READER_MAX_BOOKS];
    int n = reader_store_list(books, READER_MAX_BOOKS);
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (!books[i].builtin) ++count;
    }
    return count;
}

static esp_err_t handle_space(httpd_req_t *req)
{
    size_t total = 0, used = 0, free_b = 0;
    if (reader_store_space(&total, &used, &free_b) != ESP_OK) {
        total = used = free_b = 0;
    }
    char body[160];
    snprintf(body, sizeof(body),
             "{\"total\":%u,\"used\":%u,\"free\":%u,\"files\":%d}",
             (unsigned)total, (unsigned)used, (unsigned)free_b, count_uploaded());
    return send_json(req, 200, body);
}

static esp_err_t handle_list(httpd_req_t *req)
{
    reader_book_info_t books[READER_MAX_BOOKS];
    int n = reader_store_list(books, READER_MAX_BOOKS);
    char body[4096];
    size_t used = 0;
    used += (size_t)snprintf(body + used, sizeof(body) - used, "[");
    for (int i = 0; i < n && used + 256 < sizeof(body); ++i) {
        if (books[i].builtin) continue;
        char name[READER_BOOK_ID_MAX * 3];
        json_escape(books[i].title, name, sizeof(name));
        used += (size_t)snprintf(body + used, sizeof(body) - used,
                                 "%s{\"name\":\"%s\",\"size\":%u}",
                                 used > 1 ? "," : "", name, (unsigned)books[i].size);
    }
    if (used + 2 < sizeof(body)) body[used++] = ']';
    body[used] = '\0';
    return send_json(req, 200, body);
}

static esp_err_t handle_delete(httpd_req_t *req)
{
    char name[READER_BOOK_ID_MAX];
    if (query_value(req, "name", name, sizeof(name)) == 0) {
        (void)reader_store_delete(name);
    }
    return handle_redirect(req);
}

static esp_err_t handle_upload(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > READER_MAX_UPLOAD_BYTES) {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"zdx 文件大小必须在 1 B 到 512 KB 之间\"}");
    }
    size_t total = 0, used = 0, free_b = 0;
    if (reader_store_space(&total, &used, &free_b) == ESP_OK &&
        free_b < req->content_len + 8192) {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"剩余空间不足，请先删除部分书籍\"}");
    }

    char name[192];
    if (query_value(req, "name", name, sizeof(name)) != 0 || name[0] == '\0') {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"缺少文件名\"}");
    }
    char id[READER_BOOK_ID_MAX];
    if (reader_store_begin_upload(name, id, sizeof(id)) != ESP_OK) {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"已有上传任务或存储不可用\"}");
    }

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
        set_message("上传中断，已丢弃未完成的文件");
        return send_json(req, 400, "{\"ok\":false,\"error\":\"传输中断或写入失败\"}");
    }

    uint32_t unsupported = 0;
    if (reader_store_finish_upload(id, &unsupported) != ESP_OK) {
        set_message("保存失败，请检查剩余空间");
        return send_json(req, 400, "{\"ok\":false,\"error\":\"保存失败\"}");
    }

    char escaped[READER_BOOK_ID_MAX * 3];
    json_escape(id, escaped, sizeof(escaped));
    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"name\":\"%s\",\"size\":%u,\"unsupported\":%u}",
             escaped, (unsigned)received, (unsigned)unsupported);

    if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        snprintf(s_status.last_upload, sizeof(s_status.last_upload), "%s", id);
        s_status.last_upload_size = (uint32_t)received;
        s_status.last_unsupported = unsupported;
        s_status.total_uploads++;
        xSemaphoreGive(s_status_lock);
    }
    set_message("已收到《%s》，共 %u 字节", id, (unsigned)received);
    ESP_LOGI(TAG, "上传完成 %s (%u B, 未覆盖字符 %u)", id, (unsigned)received,
             (unsigned)unsupported);
    return send_json(req, 200, body);
}

static const httpd_uri_t URI_HANDLERS[] = {
    { .uri = "/",       .method = HTTP_GET,  .handler = handle_root,     .user_ctx = NULL },
    { .uri = "/space",  .method = HTTP_GET,  .handler = handle_space,    .user_ctx = NULL },
    { .uri = "/list",   .method = HTTP_GET,  .handler = handle_list,     .user_ctx = NULL },
    { .uri = "/delete", .method = HTTP_GET,  .handler = handle_delete,   .user_ctx = NULL },
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
        set_message("手机已连接，正在分配地址");
        ESP_LOGI(TAG, "设备已接入热点");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_status.client_count > 0) s_status.client_count--;
            s_status.client_ready = false;
            xSemaphoreGive(s_status_lock);
        }
        set_message("手机已断开，热点仍在运行");
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_status.client_ready = true;
            xSemaphoreGive(s_status_lock);
        }
        set_message("手机已就绪，可打开传书页面");
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
    set_message("热点已开启，等待手机连接");
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
