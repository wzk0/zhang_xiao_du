// reader_transfer.h — SoftAP + local web page for uploading books.
//
// The device opens an open (no password) 2.4 GHz AP, answers DNS queries with
// its own address, and serves a small HTTP page at http://192.168.4.1. The phone
// uploads a .txt file; chunks are written straight into the SPIFFS books
// partition. All slow work happens in the Wi-Fi/HTTP tasks. Nothing here touches
// LVGL: the transfer screen polls reader_transfer_get_status() from its own
// lv_timer inside the LVGL task.
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define READER_AP_SSID "Passport-Books"
#define READER_AP_URL  "http://192.168.4.1"
#define READER_MAX_UPLOAD_BYTES (512 * 1024)

typedef struct {
    bool active;
    bool client_ready;      // DHCP address assigned
    uint32_t client_count;
    uint32_t total_uploads;
    char last_message[96];
    char last_upload[64];
    uint32_t last_upload_size;
    uint32_t last_unsupported;
} reader_transfer_status_t;

// Starts AP + DNS + HTTP. Idempotent while active. Returns ESP_OK on success;
// on any failure all partially created resources are rolled back.
esp_err_t reader_transfer_start(void);

// Stops HTTP, DNS, Wi-Fi and the AP netif, then resets the status. Idempotent.
void reader_transfer_stop(void);

// Thread-safe snapshot for the UI timer.
void reader_transfer_get_status(reader_transfer_status_t *out);
