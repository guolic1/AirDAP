#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE = 512,
};

/* Host-test seams for the transport-independent RX worker. */
void airdap_target_uart_publish_rx(const uint8_t *data, size_t length);
bool airdap_target_uart_process_rx_once(void);
