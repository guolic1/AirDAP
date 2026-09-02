#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct fake_esp_timer *esp_timer_handle_t;

typedef struct {
    void (*callback)(void *argument);
    void *arg;
    const char *name;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(
    const esp_timer_create_args_t *args,
    esp_timer_handle_t *timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
bool esp_timer_is_active(esp_timer_handle_t timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
