#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "airdap_target_uart.h"
#include "airdap_usb_uart_bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

enum {
    USB_UART_TASK_STACK_SIZE = 3072,
    USB_UART_TASK_PRIORITY = 5,
    USB_UART_IO_CHUNK = 256,
    USB_UART_IDLE_POLL_MS = 1,
};

static const char *TAG = "airdap_usb_uart";
static atomic_uint usb_uart_session;
static cdc_line_coding_t current_line_coding = {
    .bit_rate = AIRDAP_TARGET_UART_DEFAULT_BAUD,
    .stop_bits = 0U,
    .parity = 0U,
    .data_bits = 8U,
};

static airdap_target_uart_session_id_t open_usb_uart_session(void)
{
    airdap_target_uart_session_id_t session =
        atomic_load(&usb_uart_session);
    if (session != 0U) {
        return session;
    }

    airdap_target_uart_session_id_t opened_session = 0U;
    const airdap_target_uart_result_t result =
        airdap_target_uart_session_open(
            AIRDAP_TARGET_UART_TRANSPORT_USB,
            false,
            &opened_session);
    if (result != AIRDAP_TARGET_UART_OK) {
        ESP_LOGW(TAG, "Unable to open target UART USB session: %u",
            (unsigned int) result);
        return 0U;
    }

    unsigned int expected = 0U;
    if (!atomic_compare_exchange_strong(
        &usb_uart_session,
        &expected,
        opened_session)) {
        (void) airdap_target_uart_session_close(
            AIRDAP_TARGET_UART_TRANSPORT_USB,
            opened_session);
        session = expected;
    } else {
        session = opened_session;
    }
    return session;
}

static void close_usb_uart_session(void)
{
    const airdap_target_uart_session_id_t session =
        atomic_load(&usb_uart_session);
    if (session == 0U) {
        return;
    }

    const airdap_target_uart_result_t result =
        airdap_target_uart_session_close(
            AIRDAP_TARGET_UART_TRANSPORT_USB,
            session);
    if (result != AIRDAP_TARGET_UART_OK &&
        result != AIRDAP_TARGET_UART_STALE_SESSION) {
        ESP_LOGW(TAG, "Unable to close target UART USB session: %u",
            (unsigned int) result);
        return;
    }
    unsigned int expected = session;
    (void) atomic_compare_exchange_strong(
        &usb_uart_session,
        &expected,
        0U);
}

static airdap_target_uart_result_t acquire_usb_uart_tx(
    airdap_target_uart_session_id_t session)
{
    return airdap_target_uart_tx_acquire(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        session);
}

static void apply_line_coding(airdap_target_uart_session_id_t session)
{
    const airdap_target_uart_result_t result =
        airdap_target_uart_session_configure(
            AIRDAP_TARGET_UART_TRANSPORT_USB,
            session,
            current_line_coding.bit_rate,
            current_line_coding.stop_bits,
            current_line_coding.parity,
            current_line_coding.data_bits);
    if (result != AIRDAP_TARGET_UART_OK) {
        ESP_LOGW(
            TAG,
            "Unsupported or unavailable CDC line coding: %u baud, %u/%u/%u (result %u)",
            (unsigned int) current_line_coding.bit_rate,
            (unsigned int) current_line_coding.data_bits,
            (unsigned int) current_line_coding.parity,
            (unsigned int) current_line_coding.stop_bits,
            (unsigned int) result);
    }
}

static void cdc_receive_callback(int interface_number, cdcacm_event_t *event)
{
    (void) event;
    uint8_t data[USB_UART_IO_CHUNK];
    size_t received = 0U;

    do {
        if (tinyusb_cdcacm_read(
            interface_number,
            data,
            sizeof(data),
            &received) != ESP_OK) {
            return;
        }
        if (received == 0U) {
            continue;
        }

        const airdap_target_uart_session_id_t session =
            open_usb_uart_session();
        if (session == 0U) {
            continue;
        }
        const airdap_target_uart_result_t acquire_result =
            acquire_usb_uart_tx(session);
        if (acquire_result != AIRDAP_TARGET_UART_OK &&
            acquire_result != AIRDAP_TARGET_UART_ALREADY_OWNER) {
            ESP_LOGW(TAG, "Target UART TX busy for USB: %u",
                (unsigned int) acquire_result);
            continue;
        }
        if (acquire_result == AIRDAP_TARGET_UART_OK) {
            apply_line_coding(session);
        }

        size_t written = 0U;
        const airdap_target_uart_result_t write_result =
            airdap_target_uart_session_write(
                AIRDAP_TARGET_UART_TRANSPORT_USB,
                session,
                data,
                received,
                &written);
        if (write_result != AIRDAP_TARGET_UART_OK || written != received) {
            ESP_LOGW(
                TAG,
                "CDC to target UART write failed: %u/%u (result %u)",
                (unsigned int) written,
                (unsigned int) received,
                (unsigned int) write_result);
            return;
        }
    } while (received == sizeof(data));
}

static void configure_usb_uart_session(void)
{
    const airdap_target_uart_session_id_t session =
        open_usb_uart_session();
    if (session == 0U) {
        return;
    }
    const airdap_target_uart_result_t acquire_result =
        acquire_usb_uart_tx(session);
    if (acquire_result != AIRDAP_TARGET_UART_OK &&
        acquire_result != AIRDAP_TARGET_UART_ALREADY_OWNER) {
        ESP_LOGW(TAG, "Target UART TX busy for CDC line coding: %u",
            (unsigned int) acquire_result);
        return;
    }
    apply_line_coding(session);
}

static void cdc_line_state_callback(
    int interface_number,
    cdcacm_event_t *event)
{
    (void) interface_number;
    if (event == NULL) {
        return;
    }
    if (event->line_state_changed_data.dtr) {
        configure_usb_uart_session();
    } else {
        close_usb_uart_session();
    }
}

static void cdc_line_coding_callback(
    int interface_number,
    cdcacm_event_t *event)
{
    (void) interface_number;
    if (event == NULL ||
        event->line_coding_changed_data.p_line_coding == NULL) {
        return;
    }
    current_line_coding =
        *event->line_coding_changed_data.p_line_coding;

    /* Hosts may configure CDC during enumeration with DTR low. Cache that
     * request until open so it cannot retain TX ownership without a close. */
    if (tud_cdc_n_connected(0U)) {
        configure_usb_uart_session();
    }
}

bool airdap_usb_uart_bridge_process_once(void)
{
    const airdap_target_uart_session_id_t session =
        atomic_load(&usb_uart_session);
    if (session == 0U || !tud_cdc_n_connected(0U)) {
        return false;
    }

    const uint32_t write_available = tud_cdc_n_write_available(0U);
    if (write_available == 0U) {
        return false;
    }

    uint8_t data[USB_UART_IO_CHUNK];
    const size_t capacity = write_available < sizeof(data)
        ? (size_t) write_available
        : sizeof(data);
    size_t received = 0U;
    const airdap_target_uart_result_t result =
        airdap_target_uart_session_read(
            AIRDAP_TARGET_UART_TRANSPORT_USB,
            session,
            data,
            capacity,
            &received);
    if (result == AIRDAP_TARGET_UART_STALE_SESSION) {
        unsigned int expected = session;
        (void) atomic_compare_exchange_strong(
            &usb_uart_session,
            &expected,
            0U);
        return false;
    }
    if (result != AIRDAP_TARGET_UART_OK || received == 0U) {
        if (result != AIRDAP_TARGET_UART_OK) {
            ESP_LOGW(TAG, "Target UART USB read failed: %u",
                (unsigned int) result);
        }
        return false;
    }

    const size_t queued = tinyusb_cdcacm_write_queue(
        TINYUSB_CDC_ACM_0,
        data,
        received);
    if (queued != received) {
        ESP_LOGW(TAG, "Target UART to CDC overflow: %u/%u",
            (unsigned int) queued,
            (unsigned int) received);
    }
    (void) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0U);
    return true;
}

static void usb_uart_worker(void *argument)
{
    (void) argument;
    const TickType_t poll_ticks = pdMS_TO_TICKS(USB_UART_IDLE_POLL_MS);
    for (;;) {
        if (!airdap_usb_uart_bridge_process_once()) {
            /* Sub-tick delays round down to zero at the default 100 Hz.
             * Block for at least one tick so the CPU idle task can run. */
            vTaskDelay(poll_ticks > 0U ? poll_ticks : 1U);
        }
    }
}

esp_err_t airdap_usb_uart_bridge_start(void)
{
    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_receive_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_callback,
        .callback_line_coding_changed = cdc_line_coding_callback,
    };
    esp_err_t error = tinyusb_cdcacm_init(&cdc_config);
    if (error != ESP_OK) {
        return error;
    }
    if (xTaskCreatePinnedToCore(
        usb_uart_worker,
        "usb_uart",
        USB_UART_TASK_STACK_SIZE,
        NULL,
        USB_UART_TASK_PRIORITY,
        NULL,
        1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void airdap_usb_uart_bridge_disconnected(void)
{
    close_usb_uart_session();
}
