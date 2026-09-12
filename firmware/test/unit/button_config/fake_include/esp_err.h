#pragma once
#include "../../config_store/fake_include/esp_err.h"
static inline const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}
