#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t airdap_dap_init(
    const char *serial_number,
    const char *firmware_version);

size_t airdap_dap_process(
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity);

#ifdef __cplusplus
}
#endif
