#pragma once
#include <stdint.h>
int64_t stub_now_us(void);
static inline int64_t esp_timer_get_time(void) { return stub_now_us(); }
