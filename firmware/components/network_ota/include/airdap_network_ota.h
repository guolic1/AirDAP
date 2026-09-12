#pragma once

#include "airdap_control.h"
#include "airdap_frame.h"
#include "airdap_network_auth.h"

/* QUERY is opcode/status/protocol/flags/capacity/running address/update address/
 * confirmed flag/version. All integers use network byte order. */
enum { AIRDAP_NETWORK_OTA_MAX_RESPONSE = 49, AIRDAP_NETWORK_OTA_MAX_DATA = 4091 };

airdap_frame_error_code_t airdap_network_ota_dispatch(
    airdap_network_auth_connection_t *connection, uint32_t session_id,
    const uint8_t *request, size_t request_size,
    uint8_t *response, size_t response_capacity, size_t *response_size);
void airdap_network_ota_disconnect(
    airdap_network_auth_connection_t *connection, uint32_t session_id);
/* Called only after a successful REBOOT response has been written to TLS. */
void airdap_network_ota_reboot(
    airdap_network_auth_connection_t *connection, uint32_t session_id);
