#include "airdap_network_uart.h"

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t) (value >> 24);
    data[1] = (uint8_t) (value >> 16);
    data[2] = (uint8_t) (value >> 8);
    data[3] = (uint8_t) value;
}

static void write_u16(uint8_t *data, size_t value)
{
    data[0] = (uint8_t) (value >> 8);
    data[1] = (uint8_t) value;
}

airdap_frame_error_code_t airdap_network_uart_error_code(
    airdap_target_uart_result_t result)
{
    switch (result) {
    case AIRDAP_TARGET_UART_OK:
    case AIRDAP_TARGET_UART_ALREADY_OWNER:
        return AIRDAP_FRAME_ERROR_NONE;
    case AIRDAP_TARGET_UART_BUSY:
    case AIRDAP_TARGET_UART_NOT_OWNER:
        return AIRDAP_FRAME_ERROR_BUSY;
    case AIRDAP_TARGET_UART_INVALID_ARGUMENT:
        return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
    case AIRDAP_TARGET_UART_UNAUTHENTICATED:
    case AIRDAP_TARGET_UART_STALE_SESSION:
        return AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    default:
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
}

airdap_frame_error_code_t airdap_network_uart_dispatch(
    airdap_network_auth_connection_t *auth_connection,
    uint32_t auth_session,
    airdap_target_uart_session_id_t uart_session,
    const uint8_t *request, size_t request_size,
    uint8_t *response, size_t response_capacity, size_t *response_size)
{
    if (response_size == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    *response_size = 0;
    if (auth_connection == NULL || auth_session == 0 || uart_session == 0 ||
        airdap_network_auth_session_validate(auth_connection, auth_session) !=
            AIRDAP_NETWORK_AUTH_OK) {
        return AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    }
    if (response == NULL || response_capacity < AIRDAP_NETWORK_UART_MAX_RESPONSE) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    if (request == NULL || request_size == 0) {
        return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
    }
    const airdap_target_uart_transport_t transport = AIRDAP_TARGET_UART_TRANSPORT_NETWORK;
    airdap_target_uart_result_t result;
    size_t count = 0;
    size_t size = 1;
    response[0] = request[0];
    switch (request[0]) {
    case AIRDAP_NETWORK_UART_STATUS: {
        if (request_size != 1) return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
        airdap_target_uart_session_status_t session_status;
        result = airdap_target_uart_session_get_status(transport, uart_session, &session_status);
        if (result != AIRDAP_TARGET_UART_OK) break;
        airdap_target_uart_status_t status;
        if (airdap_target_uart_get_status(&status) != ESP_OK) return AIRDAP_FRAME_ERROR_INTERNAL;
        write_u32(response + 1, status.baud_rate);
        response[5] = status.stop_bits;
        response[6] = status.parity;
        response[7] = status.data_bits;
        response[8] = session_status.tx_owner ? 1 : 0;
        write_u16(response + 9, session_status.rx_buffered_bytes);
        write_u32(response + 11, session_status.rx_dropped_bytes);
        size = 15;
        break;
    }
    case AIRDAP_NETWORK_UART_ACQUIRE_TX:
        if (request_size != 1) return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
        result = airdap_target_uart_tx_acquire(transport, uart_session);
        break;
    case AIRDAP_NETWORK_UART_CONFIGURE: {
        if (request_size != 8) return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
        const uint32_t baud = ((uint32_t) request[1] << 24) |
            ((uint32_t) request[2] << 16) | ((uint32_t) request[3] << 8) | request[4];
        result = airdap_target_uart_session_configure(transport, uart_session,
            baud, request[5], request[6], request[7]);
        break;
    }
    case AIRDAP_NETWORK_UART_WRITE:
        if (request_size < 2 || request_size > 1 + AIRDAP_NETWORK_UART_MAX_DATA)
            return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
        result = airdap_target_uart_session_write(transport, uart_session,
            request + 1, request_size - 1, &count);
        write_u16(response + 1, count);
        size = 3;
        break;
    case AIRDAP_NETWORK_UART_READ: {
        if (request_size != 3) return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
        const size_t capacity = ((size_t) request[1] << 8) | request[2];
        if (capacity == 0 || capacity > AIRDAP_NETWORK_UART_MAX_DATA)
            return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
        result = airdap_target_uart_session_read(transport, uart_session,
            response + 5, capacity, &count);
        if (result != AIRDAP_TARGET_UART_OK) break;
        airdap_target_uart_session_status_t status;
        result = airdap_target_uart_session_get_status(transport, uart_session, &status);
        if (result != AIRDAP_TARGET_UART_OK) break;
        write_u32(response + 1, status.rx_dropped_bytes);
        size = 5 + count;
        break;
    }
    default:
        return AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE;
    }
    const airdap_frame_error_code_t error = airdap_network_uart_error_code(result);
    if (error == AIRDAP_FRAME_ERROR_NONE) *response_size = size;
    return error;
}
