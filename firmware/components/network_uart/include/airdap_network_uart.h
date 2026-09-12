#pragma once

#include "airdap_frame.h"
#include "airdap_control.h"
#include "airdap_network_auth.h"
#include "airdap_target_uart.h"

enum {
    AIRDAP_NETWORK_UART_PORT = 3261,
    AIRDAP_NETWORK_UART_MAX_DATA = 256,
    AIRDAP_NETWORK_UART_MAX_RESPONSE = 5 + AIRDAP_NETWORK_UART_MAX_DATA,
    AIRDAP_NETWORK_UART_STATUS = AIRDAP_CONTROL_UART_STATUS,
    AIRDAP_NETWORK_UART_ACQUIRE_TX = AIRDAP_CONTROL_UART_ACQUIRE_TX,
    AIRDAP_NETWORK_UART_CONFIGURE = AIRDAP_CONTROL_UART_CONFIGURE,
    AIRDAP_NETWORK_UART_WRITE = AIRDAP_CONTROL_UART_WRITE,
    AIRDAP_NETWORK_UART_READ = AIRDAP_CONTROL_UART_READ,
};

/* CONTROL payload contract: firmware/README.md, Target UART service. This boundary
 * revalidates auth before every UART operation. Caller owns TLS framing and
 * exact UART session lifetime; AUTH must open a read-only subscriber. */
airdap_frame_error_code_t airdap_network_uart_dispatch(
    airdap_network_auth_connection_t *auth_connection,
    uint32_t auth_session,
    airdap_target_uart_session_id_t uart_session,
    const uint8_t *request, size_t request_size,
    uint8_t *response, size_t response_capacity, size_t *response_size);

airdap_frame_error_code_t airdap_network_uart_error_code(
    airdap_target_uart_result_t result);
