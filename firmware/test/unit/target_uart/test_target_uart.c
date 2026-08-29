#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_board_pins.h"
#include "airdap_target_uart.h"
#include "driver/uart.h"

enum {
    TARGET_UART_RX_BUFFER_SIZE = 2048,
    TARGET_UART_TX_BUFFER_SIZE = 2048,
};

static esp_err_t param_result = ESP_OK;
static esp_err_t pin_result = ESP_OK;
static esp_err_t install_result = ESP_OK;
static unsigned int param_calls;
static unsigned int pin_calls;
static unsigned int install_calls;
static uart_port_t configured_port;
static uart_config_t configured;
static int configured_tx;
static int configured_rx;
static int configured_rts;
static int configured_cts;
static int installed_rx_buffer;
static int installed_tx_buffer;
static int installed_queue_size;
static QueueHandle_t *installed_queue;
static int installed_flags;
static uint8_t read_payload[] = {0x12, 0x34, 0x56};
static const void *last_write_data;
static size_t last_write_size;
static TickType_t last_read_timeout;

static void reset_calls(void)
{
    param_result = ESP_OK;
    pin_result = ESP_OK;
    install_result = ESP_OK;
    param_calls = 0U;
    pin_calls = 0U;
    install_calls = 0U;
    memset(&configured, 0, sizeof(configured));
    last_write_data = NULL;
    last_write_size = 0U;
    last_read_timeout = 0U;
}

esp_err_t uart_param_config(
    uart_port_t uart_num,
    const uart_config_t *uart_config)
{
    assert(uart_config != NULL);
    ++param_calls;
    configured_port = uart_num;
    configured = *uart_config;
    return param_result;
}

esp_err_t uart_set_pin(
    uart_port_t uart_num,
    int tx_io_num,
    int rx_io_num,
    int rts_io_num,
    int cts_io_num)
{
    ++pin_calls;
    configured_port = uart_num;
    configured_tx = tx_io_num;
    configured_rx = rx_io_num;
    configured_rts = rts_io_num;
    configured_cts = cts_io_num;
    return pin_result;
}

esp_err_t uart_driver_install(
    uart_port_t uart_num,
    int rx_buffer_size,
    int tx_buffer_size,
    int queue_size,
    QueueHandle_t *uart_queue,
    int intr_alloc_flags)
{
    ++install_calls;
    configured_port = uart_num;
    installed_rx_buffer = rx_buffer_size;
    installed_tx_buffer = tx_buffer_size;
    installed_queue_size = queue_size;
    installed_queue = uart_queue;
    installed_flags = intr_alloc_flags;
    return install_result;
}

int uart_read_bytes(
    uart_port_t uart_num,
    void *buf,
    uint32_t length,
    TickType_t ticks_to_wait)
{
    assert(uart_num == UART_NUM_1);
    assert(buf != NULL);
    const size_t count = length < sizeof(read_payload) ? length : sizeof(read_payload);
    memcpy(buf, read_payload, count);
    last_read_timeout = ticks_to_wait;
    return (int) count;
}

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    assert(uart_num == UART_NUM_1);
    last_write_data = src;
    last_write_size = size;
    return (int) size;
}

static void assert_config(
    int baud,
    uart_word_length_t data_bits,
    uart_parity_t parity,
    uart_stop_bits_t stop_bits)
{
    assert(configured_port == UART_NUM_1);
    assert(configured.baud_rate == baud);
    assert(configured.data_bits == data_bits);
    assert(configured.parity == parity);
    assert(configured.stop_bits == stop_bits);
    assert(configured.flow_ctrl == UART_HW_FLOWCTRL_DISABLE);
    assert(configured.source_clk == UART_SCLK_DEFAULT);
}

static void test_io_rejected_before_initialization(void)
{
    uint8_t data = 0U;

    assert(airdap_target_uart_read(&data, 1U, 1U) == -1);
    assert(airdap_target_uart_write(&data, 1U) == -1);
}

static void test_line_coding_mapping_and_rejection(void)
{
    reset_calls();

    assert(airdap_target_uart_configure(9600U, 0U, 0U, 5U) == ESP_OK);
    assert_config(9600, UART_DATA_5_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1);

    assert(airdap_target_uart_configure(115200U, 1U, 1U, 6U) == ESP_OK);
    assert_config(115200, UART_DATA_6_BITS, UART_PARITY_ODD, UART_STOP_BITS_1_5);

    assert(airdap_target_uart_configure(1000000U, 2U, 2U, 7U) == ESP_OK);
    assert_config(1000000, UART_DATA_7_BITS, UART_PARITY_EVEN, UART_STOP_BITS_2);

    assert(airdap_target_uart_configure(5000000U, 0U, 0U, 8U) == ESP_OK);
    assert_config(5000000, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1);
    assert(param_calls == 4U);

    assert(airdap_target_uart_configure(0U, 0U, 0U, 8U) == ESP_ERR_INVALID_ARG);
    assert(airdap_target_uart_configure(5000001U, 0U, 0U, 8U) == ESP_ERR_INVALID_ARG);
    assert(airdap_target_uart_configure(115200U, 3U, 0U, 8U) == ESP_ERR_INVALID_ARG);
    assert(airdap_target_uart_configure(115200U, 0U, 3U, 8U) == ESP_ERR_INVALID_ARG);
    assert(airdap_target_uart_configure(115200U, 0U, 0U, 9U) == ESP_ERR_INVALID_ARG);
    assert(param_calls == 4U);
}

static void test_initialization_retry_and_configuration(void)
{
    reset_calls();
    param_result = ESP_FAIL;
    assert(airdap_target_uart_init() == ESP_FAIL);
    assert(param_calls == 1U && pin_calls == 0U && install_calls == 0U);

    param_result = ESP_OK;
    pin_result = ESP_FAIL;
    assert(airdap_target_uart_init() == ESP_FAIL);
    assert(param_calls == 2U && pin_calls == 1U && install_calls == 0U);

    pin_result = ESP_OK;
    assert(airdap_target_uart_init() == ESP_OK);
    assert(param_calls == 3U && pin_calls == 2U && install_calls == 1U);
    assert_config(
        AIRDAP_TARGET_UART_DEFAULT_BAUD,
        UART_DATA_8_BITS,
        UART_PARITY_DISABLE,
        UART_STOP_BITS_1);
    assert(configured_tx == AIRDAP_PIN_TARGET_TX_TDI);
    assert(configured_rx == AIRDAP_PIN_TARGET_RX_TDO);
    assert(configured_rts == UART_PIN_NO_CHANGE);
    assert(configured_cts == UART_PIN_NO_CHANGE);
    assert(installed_rx_buffer == TARGET_UART_RX_BUFFER_SIZE);
    assert(installed_tx_buffer == TARGET_UART_TX_BUFFER_SIZE);
    assert(installed_queue_size == 0);
    assert(installed_queue == NULL);
    assert(installed_flags == 0);

    assert(airdap_target_uart_init() == ESP_OK);
    assert(param_calls == 3U && pin_calls == 2U && install_calls == 1U);
}

static void test_initialized_io(void)
{
    uint8_t data[4] = {0};
    const uint8_t outbound[] = {0xAA, 0x55};

    assert(airdap_target_uart_read(data, sizeof(data), 123U) == 3);
    assert(memcmp(data, read_payload, sizeof(read_payload)) == 0);
    assert(last_read_timeout == 123U);
    assert(airdap_target_uart_write(outbound, sizeof(outbound)) == 2);
    assert(last_write_data == outbound && last_write_size == sizeof(outbound));

    assert(airdap_target_uart_read(NULL, 1U, 0U) == -1);
    assert(airdap_target_uart_read(data, 0U, 0U) == -1);
    assert(airdap_target_uart_write(NULL, 1U) == -1);
    assert(airdap_target_uart_write(outbound, 0U) == -1);
}

int main(void)
{
    test_io_rejected_before_initialization();
    test_line_coding_mapping_and_rejection();
    test_initialization_retry_and_configuration();
    test_initialized_io();

    puts("target UART tests passed");
    return 0;
}
