#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct fake_timer *esp_timer_handle_t;

typedef struct {
    void (*callback)(void *argument);
    void *arg;
    const char *name;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(
    const esp_timer_create_args_t *args,
    esp_timer_handle_t *output);
esp_err_t esp_timer_start_periodic(
    esp_timer_handle_t timer,
    uint64_t period_us);
int64_t esp_timer_get_time(void);
