#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef void (*esp_ipc_func_t)(void *context);

esp_err_t esp_ipc_call(
    uint32_t core_id,
    esp_ipc_func_t function,
    void *context);
