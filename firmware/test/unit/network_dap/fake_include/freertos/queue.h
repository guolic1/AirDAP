#pragma once

#include "freertos/FreeRTOS.h"

typedef struct fake_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(
    QueueHandle_t queue,
    const void *item,
    TickType_t ticks_to_wait);
BaseType_t xQueueReceive(
    QueueHandle_t queue,
    void *item,
    TickType_t ticks_to_wait);
void vQueueDelete(QueueHandle_t queue);
