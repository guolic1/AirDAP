#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_board_pins.h"
#include "airdap_target_uart.h"
#include "driver/uart.h"

enum {
    TARGET_UART_PORT = UART_NUM_1,
    TARGET_UART_RX_BUFFER_SIZE = 2048,
    TARGET_UART_TX_BUFFER_SIZE = 2048,
    TARGET_UART_MAX_BAUD = 5000000,
};

static bool initialized;

static bool map_data_bits(uint8_t value, uart_word_length_t *data_bits)
{
    switch (value) {
    case 5:
        *data_bits = UART_DATA_5_BITS;
        return true;
    case 6:
        *data_bits = UART_DATA_6_BITS;
        return true;
    case 7:
        *data_bits = UART_DATA_7_BITS;
        return true;
    case 8:
        *data_bits = UART_DATA_8_BITS;
        return true;
    default:
        return false;
    }
}

static bool map_stop_bits(uint8_t value, uart_stop_bits_t *stop_bits)
{
    switch (value) {
    case 0:
        *stop_bits = UART_STOP_BITS_1;
        return true;
    case 1:
        *stop_bits = UART_STOP_BITS_1_5;
        return true;
    case 2:
        *stop_bits = UART_STOP_BITS_2;
        return true;
    default:
        return false;
    }
}

static bool map_parity(uint8_t value, uart_parity_t *parity)
{
    switch (value) {
    case 0:
        *parity = UART_PARITY_DISABLE;
        return true;
    case 1:
        *parity = UART_PARITY_ODD;
        return true;
    case 2:
        *parity = UART_PARITY_EVEN;
        return true;
    default:
        return false;
    }
}

esp_err_t airdap_target_uart_configure(
    uint32_t baud_rate,
    uint8_t stop_bits,
    uint8_t parity,
    uint8_t data_bits)
{
    uart_word_length_t uart_data_bits;
    uart_stop_bits_t uart_stop_bits;
    uart_parity_t uart_parity;

    if (baud_rate == 0U || baud_rate > TARGET_UART_MAX_BAUD ||
        !map_data_bits(data_bits, &uart_data_bits) ||
        !map_stop_bits(stop_bits, &uart_stop_bits) ||
        !map_parity(parity, &uart_parity)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uart_config_t config = {
        .baud_rate = (int) baud_rate,
        .data_bits = uart_data_bits,
        .parity = uart_parity,
        .stop_bits = uart_stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    return uart_param_config(TARGET_UART_PORT, &config);
}

esp_err_t airdap_target_uart_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t error = airdap_target_uart_configure(
        AIRDAP_TARGET_UART_DEFAULT_BAUD,
        0U,
        0U,
        8U);
    if (error != ESP_OK) {
        return error;
    }

    error = uart_set_pin(
        TARGET_UART_PORT,
        AIRDAP_PIN_TARGET_TX_TDI,
        AIRDAP_PIN_TARGET_RX_TDO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if (error != ESP_OK) {
        return error;
    }

    error = uart_driver_install(
        TARGET_UART_PORT,
        TARGET_UART_RX_BUFFER_SIZE,
        TARGET_UART_TX_BUFFER_SIZE,
        0,
        NULL,
        0);
    if (error == ESP_OK) {
        initialized = true;
    }
    return error;
}

int airdap_target_uart_read(
    uint8_t *data,
    size_t capacity,
    TickType_t timeout_ticks)
{
    if (!initialized || data == NULL || capacity == 0U) {
        return -1;
    }
    return uart_read_bytes(TARGET_UART_PORT, data, capacity, timeout_ticks);
}

int airdap_target_uart_write(const uint8_t *data, size_t length)
{
    if (!initialized || data == NULL || length == 0U) {
        return -1;
    }
    return uart_write_bytes(TARGET_UART_PORT, data, length);
}
