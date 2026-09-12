#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_NETWORK_DAP_PORT = 3260,
    /* Shared bound: DAP + UART + two pending handshakes. */
    AIRDAP_NETWORK_DAP_MAX_CONNECTIONS = 4,
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

/* Starts both bounded TLS listeners (DAP 3260, UART 3261) with one registry
 * and auth revoke handler. device_identity, network_auth, mode_state,
 * target_uart and dap_service must already be initialized. Discovery may start
 * only after both listeners are ready. */
esp_err_t airdap_network_dap_start(void);

/* Copies the bounded listener registry state during one registry-lock
 * critical section. Before startup it returns an empty, not-ready snapshot;
 * TLS-only sockets are not counted as authenticated. Counts describe DAP
 * connections only; the shared authentication status includes both ports. */
esp_err_t airdap_network_dap_get_status(airdap_network_dap_status_t *status);

#ifdef __cplusplus
}
#endif
