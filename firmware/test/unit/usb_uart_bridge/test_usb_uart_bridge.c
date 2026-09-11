#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_target_uart.h"
#include "airdap_usb_uart_bridge.h"
#include "freertos/task.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

enum {
    USB_UART_TASK_STACK_SIZE = 3072,
    USB_UART_TASK_PRIORITY = 5,
};

static tinyusb_config_cdcacm_t cdc_config;
static esp_err_t cdc_init_result = ESP_OK;
static TaskFunction_t bridge_task;
static BaseType_t task_create_result = pdPASS;
static unsigned int task_create_calls;
static TickType_t last_delay;
static jmp_buf worker_yield;
static bool cdc_connected;
static uint8_t cdc_input[32];
static size_t cdc_input_length;
static unsigned int cdc_read_calls;
static uint8_t cdc_output[32];
static size_t cdc_output_length;
static uint32_t cdc_write_available = sizeof(cdc_output);
static size_t cdc_queue_limit = sizeof(cdc_output);
static unsigned int cdc_flush_calls;

static airdap_target_uart_session_id_t next_session = 41U;
static airdap_target_uart_session_id_t live_session;
static airdap_target_uart_session_id_t owner_session;
static airdap_target_uart_result_t open_result = AIRDAP_TARGET_UART_OK;
static unsigned int open_calls;
static unsigned int close_calls;
static unsigned int acquire_calls;
static unsigned int configure_calls;
static unsigned int service_read_calls;
static unsigned int service_write_calls;
static cdc_line_coding_t configured_line_coding;
static uint8_t service_rx[32];
static size_t service_rx_length;
static uint8_t service_tx[32];
static size_t service_tx_length;

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core_id)
{
    assert(task != NULL && strcmp(name, "usb_uart") == 0);
    assert(stack_depth == USB_UART_TASK_STACK_SIZE);
    assert(argument == NULL && priority == USB_UART_TASK_PRIORITY);
    assert(handle == NULL && core_id == 1);
    ++task_create_calls;
    bridge_task = task;
    return task_create_result;
}

void vTaskDelay(TickType_t ticks)
{
    last_delay = ticks;
    longjmp(worker_yield, 1);
}

esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *config)
{
    assert(config != NULL);
    cdc_config = *config;
    return cdc_init_result;
}

esp_err_t tinyusb_cdcacm_read(
    int interface_number,
    uint8_t *data,
    size_t capacity,
    size_t *received)
{
    assert(interface_number == 0 && data != NULL && received != NULL);
    ++cdc_read_calls;
    const size_t count = capacity < cdc_input_length
        ? capacity
        : cdc_input_length;
    memcpy(data, cdc_input, count);
    memmove(cdc_input, cdc_input + count, cdc_input_length - count);
    cdc_input_length -= count;
    *received = count;
    return ESP_OK;
}

size_t tinyusb_cdcacm_write_queue(
    tinyusb_cdcacm_itf_t interface_number,
    const uint8_t *data,
    size_t length)
{
    assert(interface_number == TINYUSB_CDC_ACM_0 && data != NULL);
    const size_t count = length < cdc_queue_limit ? length : cdc_queue_limit;
    memcpy(cdc_output, data, count);
    cdc_output_length = count;
    return count;
}

esp_err_t tinyusb_cdcacm_write_flush(
    tinyusb_cdcacm_itf_t interface_number,
    uint32_t timeout_ticks)
{
    assert(interface_number == TINYUSB_CDC_ACM_0 && timeout_ticks == 0U);
    ++cdc_flush_calls;
    return ESP_OK;
}

bool tud_cdc_n_connected(uint8_t interface_number)
{
    assert(interface_number == 0U);
    return cdc_connected;
}

uint32_t tud_cdc_n_write_available(uint8_t interface_number)
{
    assert(interface_number == 0U);
    return cdc_write_available;
}

airdap_target_uart_result_t airdap_target_uart_session_open(
    airdap_target_uart_transport_t transport,
    bool authenticated,
    airdap_target_uart_session_id_t *session)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_USB);
    assert(!authenticated && session != NULL);
    ++open_calls;
    if (open_result != AIRDAP_TARGET_UART_OK) {
        return open_result;
    }
    live_session = next_session++;
    *session = live_session;
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_session_close(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_USB);
    ++close_calls;
    if (session != live_session) {
        return AIRDAP_TARGET_UART_STALE_SESSION;
    }
    if (owner_session == session) {
        owner_session = 0U;
    }
    live_session = 0U;
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_tx_acquire(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_USB);
    ++acquire_calls;
    if (session != live_session) {
        return AIRDAP_TARGET_UART_STALE_SESSION;
    }
    if (owner_session == session) {
        return AIRDAP_TARGET_UART_ALREADY_OWNER;
    }
    if (owner_session != 0U) {
        return AIRDAP_TARGET_UART_BUSY;
    }
    owner_session = session;
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_session_configure(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    uint32_t baud_rate,
    uint8_t stop_bits,
    uint8_t parity,
    uint8_t data_bits)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_USB);
    assert(session == owner_session);
    ++configure_calls;
    configured_line_coding = (cdc_line_coding_t) {
        .bit_rate = baud_rate,
        .stop_bits = stop_bits,
        .parity = parity,
        .data_bits = data_bits,
    };
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_session_read(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    uint8_t *data,
    size_t capacity,
    size_t *received)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_USB);
    assert(session == live_session && data != NULL && received != NULL);
    ++service_read_calls;
    const size_t count = capacity < service_rx_length
        ? capacity
        : service_rx_length;
    memcpy(data, service_rx, count);
    memmove(service_rx, service_rx + count, service_rx_length - count);
    service_rx_length -= count;
    *received = count;
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_session_write(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    const uint8_t *data,
    size_t length,
    size_t *written)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_USB);
    assert(session == owner_session && data != NULL && written != NULL);
    ++service_write_calls;
    memcpy(service_tx, data, length);
    service_tx_length = length;
    *written = length;
    return AIRDAP_TARGET_UART_OK;
}

static void send_line_state(bool dtr)
{
    cdcacm_event_t event = {
        .type = CDC_EVENT_LINE_STATE_CHANGED,
        .line_state_changed_data = {
            .dtr = dtr,
            .rts = false,
        },
    };
    cdc_config.callback_line_state_changed(0, &event);
}

static void send_line_coding(const cdc_line_coding_t *coding)
{
    cdcacm_event_t event = {
        .type = CDC_EVENT_LINE_CODING_CHANGED,
        .line_coding_changed_data = {
            .p_line_coding = coding,
        },
    };
    cdc_config.callback_line_coding_changed(0, &event);
}

static void test_start_and_cdc_to_uart_lifecycle(void)
{
    assert(airdap_usb_uart_bridge_start() == ESP_OK);
    assert(task_create_calls == 1U && bridge_task != NULL);
    assert(cdc_config.cdc_port == TINYUSB_CDC_ACM_0);
    assert(cdc_config.callback_rx != NULL);
    assert(cdc_config.callback_line_state_changed != NULL);
    assert(cdc_config.callback_line_coding_changed != NULL);

    send_line_state(true);
    assert(open_calls == 1U && live_session != 0U);
    const cdc_line_coding_t coding = {
        .bit_rate = 1000000U,
        .stop_bits = 2U,
        .parity = 2U,
        .data_bits = 7U,
    };
    send_line_coding(&coding);
    assert(acquire_calls == 1U && configure_calls == 1U);
    assert(memcmp(&configured_line_coding, &coding, sizeof(coding)) == 0);

    static const uint8_t input[] = {0x11U, 0x22U, 0x33U};
    memcpy(cdc_input, input, sizeof(input));
    cdc_input_length = sizeof(input);
    cdc_config.callback_rx(0, NULL);
    assert(cdc_read_calls >= 1U);
    assert(service_write_calls == 1U);
    assert(service_tx_length == sizeof(input));
    assert(memcmp(service_tx, input, sizeof(input)) == 0);

    send_line_state(false);
    assert(close_calls == 1U && live_session == 0U && owner_session == 0U);
}

static void test_uart_to_cdc_and_disconnect_cleanup(void)
{
    send_line_state(true);
    cdc_connected = true;
    static const uint8_t input[] = {0xA1U, 0xA2U};
    memcpy(service_rx, input, sizeof(input));
    service_rx_length = sizeof(input);

    cdc_write_available = 0U;
    assert(!airdap_usb_uart_bridge_process_once());
    assert(service_read_calls == 0U);
    assert(service_rx_length == sizeof(input));

    cdc_write_available = 1U;
    assert(airdap_usb_uart_bridge_process_once());
    assert(service_read_calls == 1U);
    assert(service_rx_length == 1U);
    assert(cdc_output_length == 1U && cdc_output[0] == input[0]);
    assert(cdc_flush_calls == 1U);

    cdc_write_available = sizeof(cdc_output);
    assert(airdap_usb_uart_bridge_process_once());
    assert(service_read_calls == 2U);
    assert(service_rx_length == 0U);
    assert(cdc_output_length == 1U && cdc_output[0] == input[1]);
    assert(cdc_flush_calls == 2U);

    cdc_connected = false;
    assert(!airdap_usb_uart_bridge_process_once());
    airdap_usb_uart_bridge_disconnected();
    assert(close_calls == 2U && live_session == 0U);
}

static void test_busy_writer_drops_cdc_input_without_driver_access(void)
{
    send_line_state(true);
    owner_session = 999U;
    static const uint8_t input[] = {0xC1U};
    memcpy(cdc_input, input, sizeof(input));
    cdc_input_length = sizeof(input);
    const unsigned int writes_before = service_write_calls;
    cdc_config.callback_rx(0, NULL);
    assert(cdc_input_length == 0U);
    assert(service_write_calls == writes_before);
    owner_session = 0U;
    airdap_usb_uart_bridge_disconnected();
}

int main(void)
{
    test_start_and_cdc_to_uart_lifecycle();
    test_uart_to_cdc_and_disconnect_cleanup();
    test_busy_writer_drops_cdc_input_without_driver_access();
    /* Production uses 100 Hz ticks; a 1 ms poll must still block the worker
     * so the CPU idle task can run and feed its watchdog. */
    if (setjmp(worker_yield) == 0) {
        bridge_task(NULL);
        assert(false);
    }
    assert(last_delay > 0U);
    puts("USB UART bridge tests passed");
    return 0;
}
