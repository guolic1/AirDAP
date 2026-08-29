#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef int uart_port_t;
typedef int uart_word_length_t;
typedef int uart_stop_bits_t;
typedef int uart_parity_t;
typedef int uart_hw_flowcontrol_t;
typedef int uart_sclk_t;
typedef void *QueueHandle_t;

enum {
    UART_NUM_1 = 1,
    UART_DATA_5_BITS = 5,
    UART_DATA_6_BITS = 6,
    UART_DATA_7_BITS = 7,
    UART_DATA_8_BITS = 8,
    UART_STOP_BITS_1 = 10,
    UART_STOP_BITS_1_5 = 15,
    UART_STOP_BITS_2 = 20,
    UART_PARITY_DISABLE = 0,
    UART_PARITY_ODD = 1,
    UART_PARITY_EVEN = 2,
    UART_HW_FLOWCTRL_DISABLE = 0,
    UART_SCLK_DEFAULT = 0,
    UART_PIN_NO_CHANGE = -1,
};

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uart_sclk_t source_clk;
    struct {
        uint32_t allow_pd : 1;
        uint32_t backup_before_sleep : 1;
    } flags;
} uart_config_t;

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config);
esp_err_t uart_set_pin(
    uart_port_t uart_num,
    int tx_io_num,
    int rx_io_num,
    int rts_io_num,
    int cts_io_num);
esp_err_t uart_driver_install(
    uart_port_t uart_num,
    int rx_buffer_size,
    int tx_buffer_size,
    int queue_size,
    QueueHandle_t *uart_queue,
    int intr_alloc_flags);
int uart_read_bytes(
    uart_port_t uart_num,
    void *buf,
    uint32_t length,
    TickType_t ticks_to_wait);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
