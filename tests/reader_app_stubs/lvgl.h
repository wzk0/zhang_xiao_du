// Minimal LVGL stub for the host navigation test. Only the timer API is used
// by reader_app.c; every screen function is stubbed in the test itself.
#pragma once
#include <stdint.h>

typedef struct lv_timer_t { int dummy; } lv_timer_t;

static inline lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *),
                                         uint32_t period, void *user)
{
    static lv_timer_t timer;
    (void)cb; (void)period; (void)user;
    return &timer;
}

static inline void lv_timer_delete(lv_timer_t *t) { (void)t; }
