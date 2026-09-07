#pragma once

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2

static inline const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}
