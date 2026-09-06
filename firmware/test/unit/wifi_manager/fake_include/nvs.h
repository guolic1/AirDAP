#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE,
    NVS_READWRITE_PURGE,
} nvs_open_mode_t;

esp_err_t nvs_open(
    const char *namespace_name,
    nvs_open_mode_t open_mode,
    nvs_handle_t *handle);
esp_err_t nvs_purge_all(nvs_handle_t handle);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
