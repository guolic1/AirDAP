#pragma once

#include <stdbool.h>
#include <pthread.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    pthread_mutex_t mutex;
    bool initialized;
} StaticSemaphore_t;

typedef StaticSemaphore_t *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *mutex_buffer);
BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t timeout_ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
