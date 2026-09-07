#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_TARGET_UART_DEFAULT_BAUD = 115200,
};

typedef struct {
    bool initialized;
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
    size_t rx_buffered_bytes;
    size_t tx_buffer_free_bytes;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t read_failures;
    uint32_t write_failures;
} airdap_target_uart_status_t;

esp_err_t airdap_target_uart_init(void);
esp_err_t airdap_target_uart_get_status(
    airdap_target_uart_status_t *status);

esp_err_t airdap_target_uart_configure(
    uint32_t baud_rate,
    uint8_t stop_bits,
    uint8_t parity,
    uint8_t data_bits);

int airdap_target_uart_read(
    uint8_t *data,
    size_t capacity,
    TickType_t timeout_ticks);

int airdap_target_uart_write(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
