#pragma once
#include <stdint.h>
#include <stdbool.h>
static inline void bsp_display_backlight(uint8_t v) { (void)v; }
static inline bool bsp_lvgl_lock(uint32_t ms) { (void)ms; return true; }
static inline void bsp_lvgl_unlock(void) { }
