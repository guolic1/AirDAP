#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "airdap_dap_protocol.h"
#include "airdap_dap_service.h"
#include "airdap_dap_stream.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#if CONFIG_AIRDAP_DEBUG_SHELL
#include "airdap_debug_shell.h"
#endif
#include "airdap_usb.h"
#include "airdap_usb_descriptors.h"
#include "airdap_usb_status.h"
#include "airdap_usb_uart_bridge.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static const char *TAG = "airdap_usb";
static airdap_dap_stream_t dap_stream;
static uint8_t dap_usb_read_buffer[AIRDAP_DAP_BUFFER_SIZE];
static atomic_uint usb_session;
static atomic_uintptr_t next_response_token = 1U;

void airdap_usb_get_status(airdap_usb_status_t *status)
{
    if (status == NULL) {
        return;
    }
    *status = (airdap_usb_status_t) {
        .bus_mounted = tud_mounted(),
        .suspended = tud_suspended(),
        .dap_vendor_mounted = tud_vendor_n_mounted(0U),
        .target_cdc_connected = tud_cdc_n_connected(0U),
#if CONFIG_AIRDAP_DEBUG_SHELL
        .debug_vendor_mounted = tud_vendor_n_mounted(1U),
#else
        .debug_vendor_mounted = false,
#endif
        .dap_session_active = atomic_load(&usb_session) != 0U,
    };
}

static bool send_usb_response(
    void *context,
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session,
    airdap_dap_response_token_t token,
    const uint8_t *response,
    size_t response_length)
{
    (void) context;
    (void) token;
    if (transport != AIRDAP_DAP_TRANSPORT_USB ||
        session != atomic_load(&usb_session) ||
        !tud_vendor_mounted()) {
        return false;
    }

    const uint32_t written = tud_vendor_write(response, response_length);
    if (written != response_length) {
        ESP_LOGW(TAG, "DAP response truncated: %" PRIu32 "/%u", written, (unsigned) response_length);
    }
    (void) tud_vendor_write_flush();
    return written == response_length;
}

static void open_usb_session(void)
{
    if (atomic_load(&usb_session) != 0U) {
        return;
    }
    airdap_dap_session_id_t session = 0U;
    const airdap_dap_service_result_t result =
        airdap_dap_service_session_open(
            AIRDAP_DAP_TRANSPORT_USB,
            false,
            &session);
    if (result == AIRDAP_DAP_SERVICE_OK) {
        atomic_store(&usb_session, session);
    } else {
        ESP_LOGW(TAG, "Unable to open DAP USB session: %u", (unsigned) result);
    }
}

static void close_usb_session(void)
{
    const airdap_dap_session_id_t session = atomic_load(&usb_session);
    if (session == 0U) {
        return;
    }
    const airdap_dap_service_result_t result =
        airdap_dap_service_session_close(
            AIRDAP_DAP_TRANSPORT_USB,
            session);
    if (result != AIRDAP_DAP_SERVICE_OK &&
        result != AIRDAP_DAP_SERVICE_STALE_SESSION) {
        ESP_LOGW(TAG, "Unable to close DAP USB session: %u", (unsigned) result);
    }
    unsigned int expected = session;
    (void) atomic_compare_exchange_strong(&usb_session, &expected, 0U);
}

static void restart_usb_session(void)
{
    close_usb_session();
    if (tud_vendor_mounted()) {
        open_usb_session();
    }
}

static void usb_event_callback(tinyusb_event_t *event, void *argument)
{
    (void) argument;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        const airdap_mode_state_result_t result =
            airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED);
        if (result != AIRDAP_MODE_STATE_OK) {
            ESP_LOGE(TAG, "Unable to publish USB attach: %u", (unsigned) result);
            return;
        }
        open_usb_session();
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        airdap_dap_stream_init(&dap_stream);
        close_usb_session();
        airdap_usb_uart_bridge_disconnected();
        const airdap_mode_state_result_t result =
            airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_DETACHED);
        if (result != AIRDAP_MODE_STATE_OK) {
            ESP_LOGE(TAG, "Unable to publish USB detach: %u", (unsigned) result);
        }
#if CONFIG_AIRDAP_DEBUG_SHELL
        airdap_debug_shell_disconnected();
#endif
    }
}

static void enqueue_dap_request(
    void *context,
    const uint8_t *request,
    size_t request_length)
{
    (void) context;
    if (atomic_load(&usb_session) == 0U && tud_vendor_mounted()) {
        const airdap_mode_state_result_t mode_result =
            airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED);
        if (mode_result != AIRDAP_MODE_STATE_OK) {
            ESP_LOGE(
                TAG,
                "Unable to publish mounted USB state: %u",
                (unsigned) mode_result);
            return;
        }
        open_usb_session();
    }
    const airdap_dap_session_id_t session = atomic_load(&usb_session);
    if (session == 0U) {
        ESP_LOGW(TAG, "DAP request dropped without an active USB session");
        return;
    }
    const airdap_dap_service_result_t result = airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        session,
        request,
        request_length,
        atomic_fetch_add(&next_response_token, 1U),
        send_usb_response,
        NULL);
    if (result != AIRDAP_DAP_SERVICE_OK) {
        ESP_LOGW(TAG, "DAP request rejected by service: %u", (unsigned) result);
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

    if (airdap_dap_stream_expire(&dap_stream, esp_timer_get_time())) {
        ESP_LOGW(TAG, "Stale partial DAP request discarded");
        restart_usb_session();
    }

    while (tud_vendor_n_available(interface_number) > 0U) {
        const size_t received = tud_vendor_n_read(
            interface_number,
            dap_usb_read_buffer,
            sizeof(dap_usb_read_buffer));
        if (received == 0U) {
            return;
        }
        const airdap_dap_stream_result_t result = airdap_dap_stream_feed(
            &dap_stream,
            dap_usb_read_buffer,
            received,
            esp_timer_get_time(),
            enqueue_dap_request,
            NULL);
        if (result != AIRDAP_DAP_STREAM_OK) {
            if (result == AIRDAP_DAP_STREAM_OVERFLOW) {
                ESP_LOGW(TAG, "DAP receive buffer overflow");
            } else {
                ESP_LOGW(TAG, "Malformed DAP request discarded");
            }
            tud_vendor_n_read_flush(interface_number);
            restart_usb_session();
            return;
        }
    }
}

void tud_vendor_tx_cb(uint8_t interface_number, uint32_t sent_bytes)
{
#if CONFIG_AIRDAP_DEBUG_SHELL
    if (interface_number == 1U) {
        airdap_debug_shell_tx_complete(sent_bytes);
    }
#else
    (void) interface_number;
    (void) sent_bytes;
#endif
}

esp_err_t airdap_usb_init(void)
{
    const airdap_device_identity_t *identity = airdap_device_identity_get();
    if (identity == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = airdap_dap_service_init(
        identity->usb_serial,
        identity->firmware_version);
    if (error != ESP_OK) {
        return error;
    }

    airdap_dap_stream_init(&dap_stream);

    airdap_usb_descriptors_set_serial(identity->usb_serial);
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

    error = airdap_usb_uart_bridge_start();
    if (error != ESP_OK) {
        return error;
    }

#if CONFIG_AIRDAP_DEBUG_SHELL
    error = airdap_debug_shell_start();
    if (error != ESP_OK) {
        return error;
    }
    ESP_LOGI(
        TAG,
        "USB CMSIS-DAP v2 + target CDC + debug Vendor Bulk initialized, serial %s",
        identity->usb_serial);
#else
    ESP_LOGI(
        TAG,
        "USB CMSIS-DAP v2 + CDC initialized, serial %s",
        identity->usb_serial);
#endif
    return ESP_OK;
}
