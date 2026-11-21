// reader_transfer.h — SoftAP + local web page for uploading books.
//
// The device opens an open (no password) 2.4 GHz AP, answers DNS queries with
// its own address, and serves a small HTTP page at http://192.168.4.1. The phone
// converts a .txt file to .zdx; chunks are written into the SPIFFS books
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

typedef enum {
    READER_UPLOAD_IDLE,
    READER_UPLOAD_RECEIVING,
    READER_UPLOAD_PAUSED,
    READER_UPLOAD_VERIFYING,
    READER_UPLOAD_SAVED,
    READER_UPLOAD_FAILED,
} reader_upload_state_t;

typedef struct {
    bool active;
    bool client_ready;      // DHCP address assigned
    uint32_t client_count;
    reader_upload_state_t upload;
    uint8_t upload_percent;
    char upload_detail[64];  // stored book title or failure reason
    uint32_t unsupported;
} reader_transfer_status_t;

// Starts AP + DNS + HTTP. Idempotent while active. Returns ESP_OK on success;
// on any failure all partially created resources are rolled back.
esp_err_t reader_transfer_start(void);

// Stops HTTP, DNS, Wi-Fi and the AP netif, then resets the status. Idempotent.
void reader_transfer_stop(void);

// Thread-safe snapshot for the UI timer.
void reader_transfer_get_status(reader_transfer_status_t *out);
