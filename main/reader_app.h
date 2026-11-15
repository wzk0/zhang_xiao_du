// reader_app.h — application state machine and three-button navigation.
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// Button callback: only enqueues an event and returns. All storage, Wi-Fi and
// LVGL work happens in the application's own tasks/timers.
void reader_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// Call once at boot. Initializes UI, input dispatch, the 1 s tick timer and
// buttons; the LVGL operations inside are performed under bsp_lvgl_lock().
// key_cb is the BSP button callback owned by main.c (it carries the deep-sleep
// key-wake guard); passing NULL falls back to reader_app_on_key directly.
esp_err_t reader_app_init(bsp_btn_cb_t key_cb);

// Stops every task/timer/handler that can touch the UI before deleting it.
void reader_app_deinit(void);
