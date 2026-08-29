#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap.h"
#include "airdap_dap_protocol.h"
#include "airdap_target_uart.h"
#include "airdap_usb.h"
#include "airdap_usb_descriptors.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

enum {
    DAP_QUEUE_DEPTH = 4,
    DAP_WORKER_STACK_SIZE = 4096,
    DAP_WORKER_PRIORITY = 6,
    UART_TASK_STACK_SIZE = 3072,
    UART_TASK_PRIORITY = 5,
    UART_IO_CHUNK = 256,
};

typedef struct {
    size_t length;
    bool respond;
    uint8_t data[AIRDAP_DAP_BUFFER_SIZE];
} dap_work_item_t;

static const char *TAG = "airdap_usb";
static QueueHandle_t dap_queue;
static uint8_t dap_receive_buffer[AIRDAP_DAP_BUFFER_SIZE];
static size_t dap_receive_length;

static void enqueue_disconnect(void)
{
    if (dap_queue == NULL) {
        return;
    }
    (void) xQueueReset(dap_queue);
    const dap_work_item_t item = {
        .length = 1U,
        .respond = false,
        .data = {0x03},
    };
    (void) xQueueSend(dap_queue, &item, 0);
}

static void usb_event_callback(tinyusb_event_t *event, void *argument)
{
    (void) argument;
    if (event->id == TINYUSB_EVENT_DETACHED) {
        dap_receive_length = 0U;
        enqueue_disconnect();
    }
}

static void dap_worker_task(void *argument)
{
    (void) argument;
    dap_work_item_t item;
    uint8_t response[AIRDAP_DAP_BUFFER_SIZE];

    for (;;) {
        if (xQueueReceive(dap_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const size_t response_length = airdap_dap_process(
            item.data,
            item.length,
            response,
            AIRDAP_DAP_PACKET_SIZE);
        if (!item.respond || response_length == 0U || !tud_vendor_mounted()) {
            continue;
        }

        const uint32_t written = tud_vendor_write(response, response_length);
        if (written != response_length) {
            ESP_LOGW(TAG, "DAP response truncated: %" PRIu32 "/%u", written, (unsigned) response_length);
        }
        (void) tud_vendor_write_flush();
    }
}

static void consume_dap_requests(void)
{
    size_t offset = 0U;
    while (offset < dap_receive_length) {
        const size_t request_length = airdap_dap_request_size(
            dap_receive_buffer + offset,
            dap_receive_length - offset);
        if (request_length == 0U) {
            break;
        }
        if (request_length == SIZE_MAX || request_length > AIRDAP_DAP_PACKET_SIZE) {
            ESP_LOGW(TAG, "Malformed DAP request discarded");
            dap_receive_length = 0U;
            return;
        }

        dap_work_item_t item = {
            .length = request_length,
            .respond = true,
        };
        memcpy(item.data, dap_receive_buffer + offset, request_length);
        if (xQueueSend(dap_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "DAP queue full; request dropped");
        }
        offset += request_length;
    }

    if (offset > 0U) {
        dap_receive_length -= offset;
        memmove(
            dap_receive_buffer,
            dap_receive_buffer + offset,
            dap_receive_length);
    }
}

void tud_vendor_rx_cb(
    uint8_t interface_number,
    const uint8_t *buffer,
    uint16_t buffer_size)
{
    (void) buffer;
    (void) buffer_size;
    if (interface_number != 0U) {
        return;
    }

    while (tud_vendor_n_available(interface_number) > 0U) {
        const size_t capacity = sizeof(dap_receive_buffer) - dap_receive_length;
        if (capacity == 0U) {
            ESP_LOGW(TAG, "DAP receive buffer overflow");
            tud_vendor_n_read_flush(interface_number);
            dap_receive_length = 0U;
            return;
        }
        dap_receive_length += tud_vendor_n_read(
            interface_number,
            dap_receive_buffer + dap_receive_length,
            capacity);
        consume_dap_requests();
    }
}

static void cdc_receive_callback(int interface_number, cdcacm_event_t *event)
{
    (void) event;
    uint8_t data[UART_IO_CHUNK];
    size_t received = 0U;

    do {
        if (tinyusb_cdcacm_read(
            interface_number,
            data,
            sizeof(data),
            &received) != ESP_OK) {
            return;
        }
        if (received > 0U && airdap_target_uart_write(data, received) < 0) {
            ESP_LOGW(TAG, "CDC to target UART write failed");
            return;
        }
    } while (received == sizeof(data));
}

static void cdc_line_coding_callback(int interface_number, cdcacm_event_t *event)
{
    (void) interface_number;
    const cdc_line_coding_t *coding = event->line_coding_changed_data.p_line_coding;
    const esp_err_t error = airdap_target_uart_configure(
        coding->bit_rate,
        coding->stop_bits,
        coding->parity,
        coding->data_bits);
    if (error != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unsupported CDC line coding: %" PRIu32 " baud, %u%c%u",
            coding->bit_rate,
            coding->data_bits,
            coding->parity == 0U ? 'N' : coding->parity == 1U ? 'O' : 'E',
            coding->stop_bits == 2U ? 2U : 1U);
    }
}

static void target_uart_task(void *argument)
{
    (void) argument;
    uint8_t data[UART_IO_CHUNK];

    for (;;) {
        const int received = airdap_target_uart_read(
            data,
            sizeof(data),
            pdMS_TO_TICKS(20));
        if (received <= 0 || !tud_cdc_n_connected(0)) {
            continue;
        }
        const size_t queued = tinyusb_cdcacm_write_queue(
            TINYUSB_CDC_ACM_0,
            data,
            (size_t) received);
        if (queued != (size_t) received) {
            ESP_LOGW(TAG, "Target UART to CDC overflow: %u/%d", (unsigned) queued, received);
        }
        (void) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
}

static esp_err_t make_serial_number(char serial_number[17])
{
    uint8_t mac[6];
    const esp_err_t error = esp_efuse_mac_get_default(mac);
    if (error != ESP_OK) {
        return error;
    }
    const int length = snprintf(
        serial_number,
        17,
        "ADP-%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return length == 16 ? ESP_OK : ESP_FAIL;
}

esp_err_t airdap_usb_init(void)
{
    static char serial_number[17];
    esp_err_t error = make_serial_number(serial_number);
    if (error != ESP_OK) {
        return error;
    }

    error = airdap_target_uart_init();
    if (error != ESP_OK) {
        return error;
    }
    error = airdap_dap_init(serial_number);
    if (error != ESP_OK) {
        return error;
    }

    dap_queue = xQueueCreate(DAP_QUEUE_DEPTH, sizeof(dap_work_item_t));
    if (dap_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(
        dap_worker_task,
        "dap_worker",
        DAP_WORKER_STACK_SIZE,
        NULL,
        DAP_WORKER_PRIORITY,
        NULL,
        1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    airdap_usb_descriptors_set_serial(serial_number);
    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG(
        usb_event_callback,
        NULL);
    usb_config.descriptor.device = airdap_usb_device_descriptor();
    usb_config.descriptor.string = airdap_usb_string_descriptors();
    usb_config.descriptor.string_count =
        (int) airdap_usb_string_descriptor_count();
    usb_config.descriptor.full_speed_config =
        airdap_usb_configuration_descriptor();

    error = tinyusb_driver_install(&usb_config);
    if (error != ESP_OK) {
        return error;
    }

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_receive_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = cdc_line_coding_callback,
    };
    error = tinyusb_cdcacm_init(&cdc_config);
    if (error != ESP_OK) {
        return error;
    }

    if (xTaskCreatePinnedToCore(
        target_uart_task,
        "target_uart",
        UART_TASK_STACK_SIZE,
        NULL,
        UART_TASK_PRIORITY,
        NULL,
        1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "USB CMSIS-DAP v2 + CDC initialized, serial %s",
        serial_number);
    return ESP_OK;
}
