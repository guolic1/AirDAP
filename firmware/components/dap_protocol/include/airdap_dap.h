#pragma once

#include <stddef.h>
#include <stdint.h>

#include "airdap_dap_ownership.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t airdap_dap_init(
    const char *serial_number,
    const char *firmware_version);

size_t airdap_dap_process(
    airdap_dap_owner_t owner,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity);

/* Releases the owner's claim and resets only that transport's DAP state. */
void airdap_dap_session_closed(airdap_dap_owner_t owner);

#ifdef __cplusplus
}
#endif
