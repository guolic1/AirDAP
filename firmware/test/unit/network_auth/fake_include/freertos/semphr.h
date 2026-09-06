#pragma once

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned int timeout);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
