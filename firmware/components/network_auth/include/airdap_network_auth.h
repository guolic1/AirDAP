#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_NETWORK_AUTH_PSK_SIZE = 32,
    AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE = 32,
    AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE = 32,
    AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION = 1,
    AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE =
        1 + AIRDAP_NETWORK_AUTH_PSK_SIZE,
    AIRDAP_NETWORK_AUTH_TLS_HANDSHAKE_TIMEOUT_MS = 5000,
    AIRDAP_NETWORK_AUTH_MAX_PENDING_HANDSHAKES = 2,
    AIRDAP_NETWORK_AUTH_SESSION_IDLE_TIMEOUT_US = 60000000,
    AIRDAP_NETWORK_AUTH_EXPIRY_POLL_US = 1000000,
};

typedef enum {
    AIRDAP_NETWORK_AUTH_OK = 0,
    AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT,
    AIRDAP_NETWORK_AUTH_INVALID_STATE,
    AIRDAP_NETWORK_AUTH_UNSUPPORTED_VERSION,
    AIRDAP_NETWORK_AUTH_NO_CREDENTIAL,
    AIRDAP_NETWORK_AUTH_STORAGE_FAILED,
    AIRDAP_NETWORK_AUTH_NO_MEMORY,
    AIRDAP_NETWORK_AUTH_BUSY,
    AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED,
    AIRDAP_NETWORK_AUTH_UNAUTHENTICATED,
    AIRDAP_NETWORK_AUTH_EXPIRED,
    AIRDAP_NETWORK_AUTH_REPLAY,
} airdap_network_auth_result_t;

typedef struct airdap_network_auth_connection
    airdap_network_auth_connection_t;

typedef struct {
    uint32_t session_id;
    uint32_t credential_generation;
    uint8_t session_token[AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE];
    uint8_t credential_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
} airdap_network_auth_session_info_t;

typedef struct {
    bool credential_present;
    unsigned int pending_handshakes;
    bool logical_owner_active;
    unsigned int bound_connections;
} airdap_network_auth_status_t;

/* The callback must return promptly. It identifies the logical owner session
 * whose transport connections must be closed after timeout, credential
 * rotation, configuration clear, or a bound connection disconnect. */
typedef void (*airdap_network_auth_revoke_fn)(
    void *context,
    uint32_t session_id);

/* Loads the one active credential record and starts idle-session expiry. A
 * missing credential is a valid unpaired state; a malformed record fails
 * closed. config_store and device_identity must already be initialized. */
esp_err_t airdap_network_auth_init(void);

/* Copies non-secret authentication lifecycle state while holding the
 * component mutex once. Bound connections can remain non-zero briefly after
 * owner revocation until their transport teardown completes. */
esp_err_t airdap_network_auth_get_status(
    airdap_network_auth_status_t *status);

esp_err_t airdap_network_auth_set_revoke_handler(
    airdap_network_auth_revoke_fn handler,
    void *context);

/* Pairing is accepted only while the physically authorized BLE Security 2
 * window is active. Closing the window waits for an in-progress pairing
 * commit and prevents later requests from committing. */
esp_err_t airdap_network_auth_set_pairing_window_active(bool active);

/* The Security 2 endpoint passes exactly version || 32-byte PSK. A successful
 * NVS commit publishes the new generation, invalidates the old owner, and
 * returns only the non-secret SHA-256 fingerprint. Repeating the active PSK is
 * idempotent and does not advance the generation or revoke its session. */
airdap_network_auth_result_t airdap_network_auth_pair(
    const uint8_t *request,
    size_t request_size,
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE]);

/* Completes a TLS 1.3 PSK-DHE handshake before returning. The accepted socket
 * is temporarily made non-blocking and the asynchronous ESP-TLS handshake is
 * driven by poll() against an absolute timeout; its original flags are then
 * restored. No authenticated application session exists until session_bind
 * succeeds on the returned opaque connection. Failed, stale, and
 * admission-limited handshakes release all component-owned resources. */
airdap_network_auth_result_t airdap_network_auth_tls_accept(
    int socket_fd,
    airdap_network_auth_connection_t **connection);

/* These are TLS application-data I/O only. The future transport may read the
 * HELLO/AUTH exchange before binding, but it must call session_validate before
 * dispatching any privileged operation. */
ssize_t airdap_network_auth_tls_read(
    airdap_network_auth_connection_t *connection,
    void *buffer,
    size_t length);
ssize_t airdap_network_auth_tls_write(
    airdap_network_auth_connection_t *connection,
    const void *buffer,
    size_t length);

/* Bind with no token to create the sole logical owner. Additional service
 * connections join only by presenting that owner's random token. A token from
 * a released session is expired; a different token while an owner is active
 * is busy. Rebinding one TLS connection is a replay. */
airdap_network_auth_result_t airdap_network_auth_session_bind(
    airdap_network_auth_connection_t *connection,
    const uint8_t *owner_token,
    size_t owner_token_size,
    airdap_network_auth_session_info_t *session_info);

/* Revalidate immediately before any DAP, UART, power, reset, or OTA action.
 * Successful validation refreshes the owner's idle deadline. */
airdap_network_auth_result_t airdap_network_auth_session_validate(
    airdap_network_auth_connection_t *connection,
    uint32_t session_id);

/* A disconnect of any bound service connection releases the logical owner and
 * asks the transport to close every connection carrying the same token. The
 * transport must remove the connection being closed from its registry before
 * this call so the revoke callback cannot try to close it a second time. */
void airdap_network_auth_connection_close(
    airdap_network_auth_connection_t *connection);

/* Closes pairing, atomically commits AIRDAP_CONFIG_CLEAR_NETWORK, then clears
 * the active credential and revokes the owner. A failed commit leaves the
 * persistent credential, in-memory credential, and owner intact while pairing
 * remains closed until a new physically authorized window opens. */
esp_err_t airdap_network_auth_clear_network_configuration(void);

#ifdef __cplusplus
}
#endif
