#pragma once

#include "airdap_frame.h"
#include "airdap_network_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called only for a framed TCP 3260 CONTROL request. Revalidates the exact
 * logical owner before board I/O. Response storage must hold two bytes;
 * response_size is zero on error. No TLS output occurs while ownership is held. */
airdap_frame_error_code_t airdap_network_control_dispatch(
    airdap_network_auth_connection_t *connection,
    uint32_t session_id,
    const uint8_t *payload,
    size_t payload_size,
    uint8_t response[2],
    size_t *response_size);

#ifdef __cplusplus
}
#endif
