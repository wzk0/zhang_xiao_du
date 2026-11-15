#pragma once
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
typedef void *xQueueHandle;
static inline QueueHandle_t xQueueCreate(int len, int item) { (void)len; (void)item; return (QueueHandle_t)1; }
static inline int xQueueSend(QueueHandle_t q, const void *item, int timeout) { (void)q; (void)item; (void)timeout; return pdTRUE; }
static inline int xQueueReceive(QueueHandle_t q, void *item, int timeout) { (void)q; (void)item; (void)timeout; return pdFALSE; }
static inline void vQueueDelete(QueueHandle_t q) { (void)q; }
