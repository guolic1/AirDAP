#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AIRDAP_DAP_TRANSPORT_USB = 0,
    AIRDAP_DAP_TRANSPORT_NETWORK,
    AIRDAP_DAP_TRANSPORT_COUNT,
} airdap_dap_transport_t;

typedef uint32_t airdap_dap_session_id_t;
typedef uintptr_t airdap_dap_response_token_t;

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
    AIRDAP_DAP_SERVICE_UNAUTHENTICATED,
} airdap_dap_service_result_t;

airdap_dap_service_result_t airdap_dap_service_session_open(
    airdap_dap_transport_t transport,
    bool authenticated,
    airdap_dap_session_id_t *session);
airdap_dap_service_result_t airdap_dap_service_session_close(
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

#define AIRDAP_DAP_SERVICE_REQUEST_TIMEOUT_US 1000000
