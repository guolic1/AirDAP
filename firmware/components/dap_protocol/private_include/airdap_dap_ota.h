#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t airdap_dap_ota_process(
    bool debug_connected,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity);
