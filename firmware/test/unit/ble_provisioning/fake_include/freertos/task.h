#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *argument);

BaseType_t xTaskCreate(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    unsigned int priority,
    TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
