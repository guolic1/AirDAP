#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "airdap_board_pins.h"
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
#include "airdap_usb_runtime.h"
#include "airdap_usb_uart_bridge.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "device/usbd_pvt.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static const char *TAG = "airdap_usb";
static airdap_dap_stream_t dap_stream;
static uint8_t dap_usb_read_buffer[AIRDAP_DAP_BUFFER_SIZE];
static atomic_uint usb_session;
static atomic_uintptr_t next_response_token = 1U;
static atomic_bool usb_ready;
static atomic_bool mode_poll_pending;
static atomic_bool network_profile;
static bool reconnect_pending;
static int64_t reconnect_at;
static int64_t enumeration_deadline;
static SemaphoreHandle_t usb_io_mutex;

bool airdap_usb_data_ready(void)
{
    return atomic_load(&usb_ready) && !atomic_load(&network_profile) &&
        airdap_mode_state_usb_data_enabled();
}

#if CONFIG_AIRDAP_DEBUG_SHELL
/* The sole Vendor instance becomes instance zero in the shell-only profile.
 * Serialize each I/O with profile changes to prevent a stale instance from
 * sending shell bytes to the DAP endpoints after a switch back to USB. */
static uint8_t debug_instance(void) { return network_profile ? 0U : 1U; }

bool airdap_usb_debug_mounted(void)
{
    if (usb_io_mutex == NULL || xSemaphoreTake(usb_io_mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool mounted = atomic_load(&usb_ready) && tud_vendor_n_mounted(debug_instance());
    xSemaphoreGive(usb_io_mutex);
    return mounted;
}

uint32_t airdap_usb_debug_read(void *buffer, uint32_t size)
{
    if (xSemaphoreTake(usb_io_mutex, portMAX_DELAY) != pdTRUE) return 0;
    const uint32_t count = atomic_load(&usb_ready)
        ? tud_vendor_n_read(debug_instance(), buffer, size) : 0U;
    xSemaphoreGive(usb_io_mutex);
    return count;
}

uint32_t airdap_usb_debug_write(const void *buffer, uint32_t size)
{
    if (xSemaphoreTake(usb_io_mutex, portMAX_DELAY) != pdTRUE) return 0;
    const uint32_t count = atomic_load(&usb_ready)
        ? tud_vendor_n_write(debug_instance(), buffer, size) : 0U;
    xSemaphoreGive(usb_io_mutex);
    return count;
}

uint32_t airdap_usb_debug_flush(void)
{
    if (xSemaphoreTake(usb_io_mutex, portMAX_DELAY) != pdTRUE) return 0;
    const uint32_t count = atomic_load(&usb_ready)
        ? tud_vendor_n_write_flush(debug_instance()) : 0U;
    xSemaphoreGive(usb_io_mutex);
    return count;
}
#endif


void airdap_usb_get_status(airdap_usb_status_t *status)
{
    if (status == NULL) {
        return;
    }
    *status = (airdap_usb_status_t) {
        .bus_mounted = atomic_load(&usb_ready) && tud_mounted(),
        .suspended = tud_suspended(),
        .dap_vendor_mounted = airdap_usb_data_ready() && tud_vendor_n_mounted(0U),
        .target_cdc_connected = airdap_usb_data_ready() && tud_cdc_n_connected(0U),
#if CONFIG_AIRDAP_DEBUG_SHELL
        .debug_vendor_mounted = airdap_usb_debug_mounted(),
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
    if (!airdap_usb_data_ready() || transport != AIRDAP_DAP_TRANSPORT_USB ||
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
    if (!airdap_usb_data_ready() || atomic_load(&usb_session) != 0U) {
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
        if (reconnect_pending) return;
        atomic_store(&usb_ready, true);
        enumeration_deadline = 0;
        const airdap_mode_state_result_t result =
            airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED);
        if (result != AIRDAP_MODE_STATE_OK) {
            ESP_LOGE(TAG, "Unable to publish USB attach: %u", (unsigned) result);
            return;
        }
        open_usb_session();
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        atomic_store(&usb_ready, false);
        airdap_dap_stream_init(&dap_stream);
        close_usb_session();
        airdap_usb_uart_bridge_disconnected();
        const airdap_mode_state_result_t result =
            reconnect_pending ? AIRDAP_MODE_STATE_OK :
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
    if (!airdap_usb_data_ready() || interface_number != 0U) {
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
    if (atomic_load(&usb_ready) && interface_number == debug_instance()) {
        airdap_debug_shell_tx_complete(sent_bytes);
    }
#else
    (void) interface_number;
    (void) sent_bytes;
#endif
}

void airdap_usb_reconcile_mode(void *argument)
{
    (void) argument;
    const bool desired_network = !airdap_mode_state_usb_data_enabled();
    const int64_t now = esp_timer_get_time();
    if (!reconnect_pending && desired_network != network_profile) {
        /* This callback runs on TinyUSB's task, so no class callback or
         * descriptor request can overlap the disconnect/profile update. */
        if (xSemaphoreTake(usb_io_mutex, 0) != pdTRUE) goto done;
        atomic_store(&usb_ready, false);
        (void) tud_disconnect();
        reconnect_pending = true;
        reconnect_at = now + 250000;
        xSemaphoreGive(usb_io_mutex);
        close_usb_session();
        airdap_dap_stream_init(&dap_stream);
        airdap_usb_uart_bridge_disconnected();
#if CONFIG_AIRDAP_DEBUG_SHELL
        airdap_debug_shell_disconnected();
#endif
    }
    if (reconnect_pending && now >= reconnect_at) {
        if (xSemaphoreTake(usb_io_mutex, 0) != pdTRUE) goto done;
        network_profile = desired_network;
        airdap_usb_descriptors_set_network(network_profile);
        reconnect_pending = false;
        enumeration_deadline = now + 1000000;
        if (!network_profile || CONFIG_AIRDAP_DEBUG_SHELL) (void) tud_connect();
        xSemaphoreGive(usb_io_mutex);
        ESP_LOGI(TAG, "USB profile: %s", network_profile ? "network (debug only)" : "DAP + UART");
    }
    /* A cable removed during soft disconnect may not produce a second bus
     * event. Clear the stale mounted state if the host never enumerates. */
    if (enumeration_deadline != 0 && now >= enumeration_deadline && !atomic_load(&usb_ready)) {
        const airdap_mode_state_result_t result =
            airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_DETACHED);
        if (result != AIRDAP_MODE_STATE_OK) ESP_LOGE(TAG, "Unable to publish USB enumeration timeout: %u", result);
        enumeration_deadline = 0;
    }
done:
    atomic_store(&mode_poll_pending, false);
}

static void usb_mode_worker(void *argument)
{
    (void) argument;
    for (;;) {
        if (!atomic_exchange(&mode_poll_pending, true)) {
            usbd_defer_func(airdap_usb_reconcile_mode, NULL, false);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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

    usb_io_mutex = xSemaphoreCreateMutex();
    if (usb_io_mutex == NULL) return ESP_ERR_NO_MEM;
    airdap_usb_descriptors_set_network(false);
    airdap_usb_descriptors_set_serial(identity->usb_serial);
    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG(
        usb_event_callback,
        NULL);
    /* External power keeps the S3 alive after unplug; monitor the board's
     * VBUS divider so the PHY reports detach instead of only bus suspend. */
    usb_config.phy.self_powered = true;
    usb_config.phy.vbus_monitor_io = AIRDAP_PIN_USB_VBUS_SENSE;
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
    if (xTaskCreate(usb_mode_worker, "usb_mode", 2048, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
