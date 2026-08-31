#pragma once

#include <pthread.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    pthread_mutex_t mutex;
} fake_semaphore_t;

typedef fake_semaphore_t *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t timeout_ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
