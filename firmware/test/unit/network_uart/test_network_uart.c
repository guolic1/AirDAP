/* Reuse the real service's driver fixture and its public-contract regression. */
#define main target_uart_baseline_main
#include "../target_uart/test_target_uart.c"
#undef main
#include "airdap_network_uart.h"

static bool authorized = true;
static unsigned int validations;
airdap_network_auth_result_t airdap_network_auth_session_validate(
    airdap_network_auth_connection_t *connection, uint32_t session)
{
    assert(connection != NULL && session == 7U);
    ++validations;
    return authorized ? AIRDAP_NETWORK_AUTH_OK : AIRDAP_NETWORK_AUTH_EXPIRED;
}

static uint8_t response[300];
static size_t response_size;
static airdap_frame_error_code_t dispatch(uint32_t session,
    const uint8_t *request, size_t size)
{
    return airdap_network_uart_dispatch((void *) &authorized, 7U, session,
        request, size, response, sizeof(response), &response_size);
}

int main(void)
{
    assert(target_uart_baseline_main() == 0);
    uint32_t usb, net;
    open_pair(&usb, &net);
    const uint8_t acquire[] = {0x11};
    const uint8_t config[] = {0x12, 0x00, 0x01, 0xC2, 0x00, 0, 0, 8};
    const uint8_t status[] = {0x10};
    const uint8_t write[] = {0x13, 0xA5, 0x00, 0xFF};
    const uint8_t read[] = {0x14, 0x01, 0x00};
    assert(dispatch(net, config, sizeof(config)) == AIRDAP_FRAME_ERROR_BUSY);
    assert(dispatch(net, acquire, 1) == AIRDAP_FRAME_ERROR_NONE);
    assert(response_size == 1 && response[0] == 0x11);
    assert(dispatch(net, config, sizeof(config)) == AIRDAP_FRAME_ERROR_NONE);
    assert(dispatch(net, status, 1) == AIRDAP_FRAME_ERROR_NONE);
    const uint8_t golden[] = {0x10, 0, 1, 0xC2, 0, 0, 0, 8, 1, 0, 0, 0, 0, 0, 0};
    assert(response_size == sizeof(golden));
    assert(memcmp(response, golden, sizeof(golden)) == 0);
    assert(airdap_target_uart_tx_acquire(AIRDAP_TARGET_UART_TRANSPORT_USB, usb)
        == AIRDAP_TARGET_UART_BUSY);
    uint8_t invalid[sizeof(config)];
    memcpy(invalid, config, sizeof(config));
    for (size_t i = 5; i < sizeof(config); ++i) {
        invalid[i] = 255;
        assert(dispatch(net, invalid, sizeof(invalid)) == AIRDAP_FRAME_ERROR_INVALID_ARGUMENT);
        invalid[i] = config[i];
    }
    assert(dispatch(net, status, 1) == AIRDAP_FRAME_ERROR_NONE);
    assert(memcmp(response, golden, sizeof(golden)) == 0);
    assert(dispatch(net, write, sizeof(write)) == AIRDAP_FRAME_ERROR_NONE);
    const uint8_t accepted[] = {0x13, 0, 2}; /* fixture accepts a partial write */
    assert(response_size == 3 && memcmp(response, accepted, 3) == 0);
    tx_buffer_free_bytes = 0;
    assert(dispatch(net, write, sizeof(write)) == AIRDAP_FRAME_ERROR_NONE);
    assert(response_size == 3 && response[1] == 0 && response[2] == 0);
    tx_buffer_free_bytes = 1900;
    authorized = false;
    const unsigned int writes_before = param_calls;
    assert(dispatch(net, config, sizeof(config)) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(dispatch(net, write, sizeof(write)) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(dispatch(net, read, sizeof(read)) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(param_calls == writes_before && response_size == 0);
    authorized = true;
    uint8_t bytes[256]; memset(bytes, 0x5A, sizeof(bytes));
    uint8_t usb_bytes[256]; size_t received;
    for (unsigned int i = 0; i < 3; ++i) {
        airdap_target_uart_publish_rx(bytes, sizeof(bytes));
        assert(airdap_target_uart_session_read(AIRDAP_TARGET_UART_TRANSPORT_USB,
            usb, usb_bytes, sizeof(usb_bytes), &received) == AIRDAP_TARGET_UART_OK);
        assert(received == 256 && memcmp(bytes, usb_bytes, 256) == 0);
    }
    assert(dispatch(net, read, sizeof(read)) == AIRDAP_FRAME_ERROR_NONE);
    const uint8_t read_prefix[] = {0x14, 0, 0, 1, 0};
    assert(response_size == 261 && memcmp(response, read_prefix, 5) == 0);
    assert(memcmp(response + 5, bytes, 256) == 0);
    const uint8_t zero_read[] = {0x14, 0, 0};
    assert(dispatch(net, zero_read, 3) == AIRDAP_FRAME_ERROR_INVALID_ARGUMENT);
    assert(dispatch(net, config, 7) == AIRDAP_FRAME_ERROR_INVALID_ARGUMENT);
    const uint8_t unknown[] = {0xFF};
    assert(dispatch(net, unknown, 1) == AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
    assert(airdap_target_uart_session_close(AIRDAP_TARGET_UART_TRANSPORT_NETWORK, net)
        == AIRDAP_TARGET_UART_OK);
    assert(dispatch(net, write, sizeof(write)) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(airdap_target_uart_tx_acquire(AIRDAP_TARGET_UART_TRANSPORT_USB, usb)
        == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_session_close(AIRDAP_TARGET_UART_TRANSPORT_USB, usb)
        == AIRDAP_TARGET_UART_OK);
    assert(validations > 10);
    puts("network UART golden payloads, auth, ownership and fan-out passed");
    return 0;
}
