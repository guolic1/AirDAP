#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_config_store.h"
#include "airdap_device_identity.h"
#include "airdap_voltage_monitor.h"
#include "esp_err.h"

typedef enum {
    CALL_MODE_STATE_INITIALIZE,
    CALL_OTA_INITIALIZE,
    CALL_BOARD_INITIALIZE,
    CALL_DEVICE_IDENTITY_INITIALIZE,
    CALL_CONFIG_STORE_INITIALIZE,
    CALL_VOLTAGE_INITIALIZE,
    CALL_SWD_INITIALIZE,
    CALL_VOLTAGE_READ,
    CALL_USB_INITIALIZE,
    CALL_OTA_CONFIRM,
    CALL_DEFAULT_EVENT_LOOP_CREATE,
    CALL_WIFI_MANAGER_START,
    CALL_DISCOVERY_START,
    CALL_BLE_PROVISIONING_START,
} call_t;

static call_t calls[14];
static size_t call_count;
static esp_err_t wifi_start_result = ESP_OK;
static esp_err_t discovery_start_result = ESP_OK;

static void record(call_t call)
{
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    calls[call_count++] = call;
}

void airdap_mode_state_init(void)
{
    record(CALL_MODE_STATE_INITIALIZE);
}

esp_err_t airdap_ota_initialize(void)
{
    record(CALL_OTA_INITIALIZE);
    return ESP_OK;
}

esp_err_t airdap_board_init_safe(void)
{
    record(CALL_BOARD_INITIALIZE);
    return ESP_OK;
}

esp_err_t airdap_device_identity_init(void)
{
    record(CALL_DEVICE_IDENTITY_INITIALIZE);
    return ESP_OK;
}

esp_err_t airdap_config_store_init(void)
{
    record(CALL_CONFIG_STORE_INITIALIZE);
    return ESP_OK;
}

esp_err_t airdap_voltage_monitor_init(void)
{
    record(CALL_VOLTAGE_INITIALIZE);
    return ESP_OK;
}

esp_err_t airdap_swd_init(uint32_t clock_hz)
{
    assert(clock_hz == 1000000U);
    record(CALL_SWD_INITIALIZE);
    return ESP_OK;
}

esp_err_t airdap_voltage_monitor_read(airdap_voltage_reading_t *reading)
{
    assert(reading != NULL);
    record(CALL_VOLTAGE_READ);
    reading->target_mv = 3300U;
    reading->usb_vbus_mv = 5000U;
    return ESP_OK;
}

esp_err_t airdap_usb_init(void)
{
    record(CALL_USB_INITIALIZE);
    return ESP_OK;
}

esp_err_t airdap_ota_confirm_running_image(void)
{
    record(CALL_OTA_CONFIRM);
    return ESP_OK;
}

esp_err_t esp_event_loop_create_default(void)
{
    record(CALL_DEFAULT_EVENT_LOOP_CREATE);
    return ESP_OK;
}

esp_err_t airdap_wifi_manager_start(void)
{
    record(CALL_WIFI_MANAGER_START);
    return wifi_start_result;
}

esp_err_t airdap_discovery_start(void)
{
    record(CALL_DISCOVERY_START);
    return discovery_start_result;
}

esp_err_t airdap_ble_provisioning_start(void)
{
    record(CALL_BLE_PROVISIONING_START);
    return ESP_FAIL;
}

void app_main(void);

static void test_wifi_failure_does_not_start_discovery(void)
{
    static const call_t expected[] = {
        CALL_MODE_STATE_INITIALIZE,
        CALL_OTA_INITIALIZE,
        CALL_BOARD_INITIALIZE,
        CALL_DEVICE_IDENTITY_INITIALIZE,
        CALL_CONFIG_STORE_INITIALIZE,
        CALL_VOLTAGE_INITIALIZE,
        CALL_SWD_INITIALIZE,
        CALL_VOLTAGE_READ,
        CALL_USB_INITIALIZE,
        CALL_OTA_CONFIRM,
        CALL_DEFAULT_EVENT_LOOP_CREATE,
        CALL_WIFI_MANAGER_START,
        CALL_BLE_PROVISIONING_START,
    };

    wifi_start_result = ESP_FAIL;
    discovery_start_result = ESP_OK;
    app_main();

    assert(call_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t index = 0U; index < call_count; ++index) {
        assert(calls[index] == expected[index]);
    }
}

static void test_discovery_starts_after_wifi_and_does_not_block_startup(void)
{
    static const call_t expected[] = {
        CALL_MODE_STATE_INITIALIZE,
        CALL_OTA_INITIALIZE,
        CALL_BOARD_INITIALIZE,
        CALL_DEVICE_IDENTITY_INITIALIZE,
        CALL_CONFIG_STORE_INITIALIZE,
        CALL_VOLTAGE_INITIALIZE,
        CALL_SWD_INITIALIZE,
        CALL_VOLTAGE_READ,
        CALL_USB_INITIALIZE,
        CALL_OTA_CONFIRM,
        CALL_DEFAULT_EVENT_LOOP_CREATE,
        CALL_WIFI_MANAGER_START,
        CALL_DISCOVERY_START,
        CALL_BLE_PROVISIONING_START,
    };

    call_count = 0U;
    wifi_start_result = ESP_OK;
    discovery_start_result = ESP_FAIL;
    app_main();
    assert(call_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t index = 0U; index < call_count; ++index) {
        assert(calls[index] == expected[index]);
    }
}

int main(void)
{
    test_wifi_failure_does_not_start_discovery();
    test_discovery_starts_after_wifi_and_does_not_block_startup();
    puts("app_main initialization-order test passed");
    return 0;
}
