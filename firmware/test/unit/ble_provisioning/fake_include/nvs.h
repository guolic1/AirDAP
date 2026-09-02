#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef unsigned int nvs_handle_t;
typedef enum {
    NVS_READONLY = 0,
} nvs_open_mode_t;

esp_err_t nvs_open_from_partition(
    const char *partition_name,
    const char *namespace_name,
    nvs_open_mode_t open_mode,
    nvs_handle_t *handle);
esp_err_t nvs_get_blob(
    nvs_handle_t handle,
    const char *key,
    void *output,
    size_t *length);
void nvs_close(nvs_handle_t handle);
