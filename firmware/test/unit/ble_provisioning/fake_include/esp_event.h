#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data);

#define ESP_EVENT_ANY_ID (-1)
#define ESP_EVENT_DEFINE_BASE(name) esp_event_base_t name = #name

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void *event_handler_arg,
    esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_instance_t instance);
esp_err_t esp_event_post(
    esp_event_base_t event_base,
    int32_t event_id,
    const void *event_data,
    size_t event_data_size,
    uint32_t ticks_to_wait);
