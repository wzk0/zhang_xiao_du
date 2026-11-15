#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
static inline int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int prio, TaskHandle_t *out) { (void)fn; (void)name; (void)stack; (void)arg; (void)prio; if (out) *out = (TaskHandle_t)1; return pdPASS; }
static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
