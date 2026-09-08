#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_NETWORK_DAP_PORT = 3260,
    AIRDAP_NETWORK_DAP_MAX_CONNECTIONS = 3,
    AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE = 36,
    AIRDAP_NETWORK_DAP_AUTH_RESPONSE_SIZE = 36,
};

/* Starts the bounded TLS listener. device_identity, network_auth, mode_state,
 * and dap_service must already be initialized. */
esp_err_t airdap_network_dap_start(void);

#ifdef __cplusplus
}
#endif
