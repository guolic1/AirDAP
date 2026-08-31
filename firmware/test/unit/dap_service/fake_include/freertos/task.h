#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *argument);
typedef void *TaskHandle_t;

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core_id);
