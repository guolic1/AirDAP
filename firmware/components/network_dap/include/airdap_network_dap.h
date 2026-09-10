#pragma once

#include <stdbool.h>

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

typedef struct {
    bool listener_ready;
    unsigned int allocated_connections;
    unsigned int tls_connections;
    unsigned int authenticated_connections;
    unsigned int dap_sessions;
} airdap_network_dap_status_t;

/* Starts the bounded TLS listener. device_identity, network_auth, mode_state,
 * and dap_service must already be initialized. */
esp_err_t airdap_network_dap_start(void);

/* Copies the bounded listener registry state during one registry-lock
 * critical section. Before startup it returns an empty, not-ready snapshot;
 * TLS-only sockets are not counted as authenticated. */
esp_err_t airdap_network_dap_get_status(airdap_network_dap_status_t *status);

#ifdef __cplusplus
}
#endif
