// main.c — eBook reader entry point.
//
// Boots straight into the redesigned shelf UI; there is no baseline test menu.
// Audio is deliberately NOT initialized: this application never plays sound,
// and leaving the codec/I2S resources unused keeps the largest free heap block
// available for Wi-Fi + HTTP when the transfer page opens.
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "key_guard.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "reader_app.h"
#include "reader_fonts.h"
#include "reader_store.h"

#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "reader";

// Waking from deep sleep by key reboots the chip while that key is still held.
// The button component starts a few hundred milliseconds later, registers a
// press that is already down, and grades it a long press after its 500 ms
// threshold, firing an unintended action. Drop key actions until that key is
// released. The state machine lives in key_guard.h so it can be unit-tested
// without the rest of the application.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;
static volatile bool s_input_ready;

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    // Only the NVS partition is reset here, and only for an incompatible/blank
    // namespace. This is not a full-chip erase. A merged 0x0 flash already
    // clears NVS, so no user data survives that path anyway.
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化");
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

// Button callbacks run on the shared esp_timer task: keep this path short and
// never touch LVGL or the filesystem here.
void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after
    // the post-init voltage sample but before input dispatch is ready, and
    // dropping it here would leave the guard armed to swallow the next press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE,
                          esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手，按键恢复响应");
        }
        return;
    }
    if (!s_input_ready) return;
    reader_app_on_key(btn, ev, user);
}

void app_main(void)
{
    ESP_LOGI(TAG, "掌小读 · 电子书阅读器启动 (ESP32-C3, %d MB Flash)", 8);
    ESP_LOGI(TAG, "启动免费堆 %u B", (unsigned)esp_get_free_heap_size());

    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒：先屏蔽按键动作，直到那颗键松手");
    }

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    // Battery is optional: a missing CW2017 degrades to "电 --" in the UI.
    (void)bsp_battery_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败 (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        reader_fonts_init();  // writable descriptors + Montserrat fallback
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL 加锁失败，无法初始化字体");
        return;
    }

    err = reader_store_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "书籍存储初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = reader_app_init(on_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用初始化失败: %s", esp_err_to_name(err));
        return;
    }

    // If the wake key was released before the button driver started sampling,
    // no release event will ever arrive and the guard would block input until
    // its deadline. Sync once against the physical level instead.
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手，提前撤防");
        }
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "就绪，免费堆 %u B", (unsigned)esp_get_free_heap_size());
}
