#pragma once

typedef int esp_err_t;

enum {
    ESP_OK = 0,
    ESP_FAIL = -1,
    ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102,
    ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_INVALID_RESPONSE = 0x108,
    ESP_ERR_NOT_FOUND = 0x105,
    ESP_ERR_WIFI_NOT_CONNECT = 0x3006,
};
