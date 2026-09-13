#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "airdap_usb.h"
#include "airdap_usb_runtime.h"
#include "airdap_usb_descriptors.h"
#include "airdap_usb_status.h"
#include "airdap_dap_service.h"
#include "airdap_dap_stream.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#include "tinyusb.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "device/usbd_pvt.h"

static bool data_enabled = true, mounted;
static bool mutex_locked;
static int64_t now;
static unsigned disconnects, connects, opened, closed, uart_closed, shell_closed, detach_events;
static uint8_t last_instance;
static tinyusb_config_t config;
static const airdap_device_identity_t identity = {
    .usb_serial = "ADP-001122334455", .firmware_version = "test",
};
const airdap_device_identity_t *airdap_device_identity_get(void) { return &identity; }
bool airdap_mode_state_usb_data_enabled(void) { return data_enabled; }
airdap_mode_state_result_t airdap_mode_state_transition(airdap_mode_event_t event)
{
    if (event == AIRDAP_MODE_EVENT_USB_DETACHED) ++detach_events;
    return AIRDAP_MODE_STATE_OK;
}
int64_t esp_timer_get_time(void) { return now; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t) &mutex_locked; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    (void) timeout; assert(sem != NULL); assert(!mutex_locked); mutex_locked = true; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem != NULL && mutex_locked); mutex_locked = false; return pdTRUE;
}
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t size,
    void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    (void) size; (void) priority; assert(task && strcmp(name, "usb_mode") == 0);
    assert(arg == NULL && handle == NULL); return pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void) ticks; assert(false); }
void usbd_defer_func(osal_task_func_t func, void *param, bool in_isr)
{
    assert(!in_isr); func(param);
}
esp_err_t tinyusb_driver_install(const tinyusb_config_t *value) { config = *value; return ESP_OK; }
bool tud_mounted(void) { return mounted; }
bool tud_suspended(void) { return false; }
bool tud_cdc_n_connected(uint8_t itf) { assert(itf == 0); return mounted; }
bool tud_vendor_n_mounted(uint8_t itf) { last_instance = itf; return mounted; }
bool tud_disconnect(void) { ++disconnects; mounted = false; return true; }
bool tud_connect(void) { ++connects; return true; }
uint32_t tud_vendor_n_available(uint8_t itf) { (void) itf; return 0; }
uint32_t tud_vendor_n_read(uint8_t itf, void *data, uint32_t length)
{
    (void) data; last_instance = itf; return length;
}
void tud_vendor_n_read_flush(uint8_t itf) { (void) itf; }
uint32_t tud_vendor_n_write(uint8_t itf, const void *data, uint32_t length)
{
    (void) data; last_instance = itf; return length;
}
uint32_t tud_vendor_n_write_flush(uint8_t itf) { last_instance = itf; return 0; }
bool tud_control_xfer(uint8_t port, const tusb_control_request_t *request, void *data, uint16_t length)
{
    (void) port; (void) request; (void) data; (void) length; return true;
}
esp_err_t airdap_dap_service_init(const char *serial, const char *version)
{
    assert(serial && version); return ESP_OK;
}
airdap_dap_service_result_t airdap_dap_service_session_open(airdap_dap_transport_t transport,
    bool authenticated, airdap_dap_session_id_t *session)
{
    assert(transport == AIRDAP_DAP_TRANSPORT_USB && !authenticated);
    *session = ++opened; return AIRDAP_DAP_SERVICE_OK;
}
airdap_dap_service_result_t airdap_dap_service_session_close(airdap_dap_transport_t transport,
    airdap_dap_session_id_t session)
{
    assert(transport == AIRDAP_DAP_TRANSPORT_USB && session != 0); ++closed;
    return AIRDAP_DAP_SERVICE_OK;
}
airdap_dap_service_result_t airdap_dap_service_submit(airdap_dap_transport_t transport,
    airdap_dap_session_id_t session, const uint8_t *request, size_t size,
    airdap_dap_response_token_t token, airdap_dap_response_fn callback, void *context)
{
    (void) transport; (void) session; (void) request; (void) size;
    (void) token; (void) callback; (void) context; assert(false); return AIRDAP_DAP_SERVICE_OK;
}
void airdap_dap_stream_init(airdap_dap_stream_t *stream) { memset(stream, 0, sizeof(*stream)); }
bool airdap_dap_stream_expire(airdap_dap_stream_t *stream, int64_t time)
{
    (void) stream; (void) time; return false;
}
airdap_dap_stream_result_t airdap_dap_stream_feed(airdap_dap_stream_t *stream,
    const uint8_t *data, size_t size, int64_t time,
    airdap_dap_stream_request_fn callback, void *context)
{
    (void) stream; (void) data; (void) size; (void) time;
    (void) callback; (void) context; assert(false); return AIRDAP_DAP_STREAM_OK;
}
esp_err_t airdap_usb_uart_bridge_start(void) { return ESP_OK; }
void airdap_usb_uart_bridge_disconnected(void) { ++uart_closed; }
esp_err_t airdap_debug_shell_start(void) { return ESP_OK; }
void airdap_debug_shell_disconnected(void) { ++shell_closed; }
void airdap_debug_shell_tx_complete(uint32_t length) { (void) length; }
static void attach(void)
{
    mounted = true;
    tinyusb_event_t event = {.id = TINYUSB_EVENT_ATTACHED}; config.event_cb(&event, NULL);
}
int main(void)
{
    assert(airdap_usb_init() == ESP_OK);
    assert(config.phy.self_powered);
    assert(config.phy.vbus_monitor_io == 8);
    assert(config.descriptor.device->idProduct == 0x4021);
    attach();
    assert(airdap_usb_data_ready() && opened == 1);
    data_enabled = false;
    assert(!airdap_usb_data_ready());
    airdap_usb_reconcile_mode(NULL);
    assert(disconnects == 1 && closed == 1 && uart_closed == 1);
    now = 249999; airdap_usb_reconcile_mode(NULL); assert(connects == 0);
    now = 250000; airdap_usb_reconcile_mode(NULL);
    assert(connects == CONFIG_AIRDAP_DEBUG_SHELL);
#if CONFIG_AIRDAP_DEBUG_SHELL
    assert(config.descriptor.full_speed_config[4] == 1);
    assert(shell_closed == 1);
    attach();
    assert(airdap_usb_debug_mounted());
    assert(airdap_usb_debug_write("x", 1) == 1 && last_instance == 0);
    airdap_usb_status_t status; airdap_usb_get_status(&status);
    assert(!status.dap_vendor_mounted && !status.target_cdc_connected && status.debug_vendor_mounted);
#endif
    assert(opened == 1 && !airdap_usb_data_ready());
    /* Changing policy alone must not treat shell instance zero as DAP. */
    data_enabled = true; assert(!airdap_usb_data_ready());
    airdap_usb_reconcile_mode(NULL);
#if CONFIG_AIRDAP_DEBUG_SHELL
    assert(airdap_usb_debug_write("x", 1) == 0);
#endif
    now += 250000; airdap_usb_reconcile_mode(NULL);
    assert(config.descriptor.device->idProduct == 0x4021);
    assert(config.descriptor.full_speed_config[4] == AIRDAP_USB_INTERFACE_COUNT);
    attach(); assert(airdap_usb_data_ready() && opened == 2);
#if CONFIG_AIRDAP_DEBUG_SHELL
    assert(airdap_usb_debug_write("x", 1) == 1 && last_instance == 1);
#endif
    /* A rapid reversal uses the latest policy and leaves the wired profile. */
    data_enabled = false; airdap_usb_reconcile_mode(NULL);
    data_enabled = true; now += 250000; airdap_usb_reconcile_mode(NULL);
    assert(config.descriptor.device->idProduct == 0x4021);
    /* No new enumeration: the cable may have been removed mid-switch. */
    now += 1000000; airdap_usb_reconcile_mode(NULL);
    assert(detach_events == 1);
    puts("USB profile lifecycle tests passed");
    return 0;
}
