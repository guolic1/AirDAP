#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_DAP_SERVICE_REQUEST_TIMEOUT_US = 1000000,
};

typedef enum {
    AIRDAP_DAP_TRANSPORT_USB = 0,
    AIRDAP_DAP_TRANSPORT_NETWORK,
    AIRDAP_DAP_TRANSPORT_COUNT,
} airdap_dap_transport_t;

typedef uint32_t airdap_dap_session_id_t;
typedef uintptr_t airdap_dap_response_token_t;

/* The transport must revalidate session/token immediately before output and
 * return false if its connection disappeared after service dispatch. The
 * callback runs inside the transport's service-session critical section, so
 * it must return promptly and must not synchronously open, close, or reset the
 * same transport's service session or submit work for that transport. */
typedef bool (*airdap_dap_response_fn)(
    void *context,
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session,
    airdap_dap_response_token_t token,
    const uint8_t *response,
    size_t response_length);

typedef enum {
    AIRDAP_DAP_SERVICE_OK = 0,
    AIRDAP_DAP_SERVICE_INVALID_ARGUMENT,
    AIRDAP_DAP_SERVICE_INVALID_STATE,
    AIRDAP_DAP_SERVICE_BUSY,
    AIRDAP_DAP_SERVICE_STALE_SESSION,
    AIRDAP_DAP_SERVICE_QUEUE_FULL,
} airdap_dap_service_result_t;

typedef struct {
    uint32_t requests_accepted;
    uint32_t requests_processed;
    uint32_t responses_delivered;
    uint32_t queue_full;
    uint32_t timed_out;
    uint32_t stale_requests;
    uint32_t stale_responses;
    uint32_t delivery_failures;
} airdap_dap_service_stats_t;

esp_err_t airdap_dap_service_init(
    const char *serial_number,
    const char *firmware_version);

airdap_dap_service_result_t airdap_dap_service_session_open(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t *session);

airdap_dap_service_result_t airdap_dap_service_session_close(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session);

/* Resets DAP state without invalidating the transport session. */
airdap_dap_service_result_t airdap_dap_service_session_reset(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session);

airdap_dap_service_result_t airdap_dap_service_submit(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session,
    const uint8_t *request,
    size_t request_length,
    airdap_dap_response_token_t response_token,
    airdap_dap_response_fn response_callback,
    void *response_context);

void airdap_dap_service_get_stats(
    airdap_dap_service_stats_t *stats);

#ifdef __cplusplus
}
#endif
