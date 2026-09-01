#pragma once

#include "esp_err.h"

#define ESP_LOGE(tag, ...) ((void) (tag))
#define ESP_LOGI(tag, ...) ((void) (tag))
#define ESP_LOGW(tag, ...) ((void) (tag))

static inline const char *esp_err_to_name(esp_err_t error)
{
    (void) error;
    return "fake-error";
}
