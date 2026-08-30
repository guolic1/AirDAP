#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_device_identity.h"
#include "airdap_voltage_monitor.h"
#include "esp_err.h"

typedef enum {
    CALL_OTA_INITIALIZE,
    CALL_BOARD_INITIALIZE,
    CALL_DEVICE_IDENTITY_INITIALIZE,
    CALL_VOLTAGE_INITIALIZE,
    CALL_SWD_INITIALIZE,
    CALL_VOLTAGE_READ,
    CALL_USB_INITIALIZE,
    CALL_OTA_CONFIRM,
} call_t;

static call_t calls[8];
static size_t call_count;

static void record(call_t call)
{
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    calls[call_count++] = call;
}

void airdap_ota_initialize(void)
{
    record(CALL_OTA_INITIALIZE);
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

void app_main(void);

int main(void)
{
    static const call_t expected[] = {
        CALL_OTA_INITIALIZE,
        CALL_BOARD_INITIALIZE,
        CALL_DEVICE_IDENTITY_INITIALIZE,
        CALL_VOLTAGE_INITIALIZE,
        CALL_SWD_INITIALIZE,
        CALL_VOLTAGE_READ,
        CALL_USB_INITIALIZE,
        CALL_OTA_CONFIRM,
    };

    app_main();

    assert(call_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t index = 0U; index < call_count; ++index) {
        assert(calls[index] == expected[index]);
    }
    puts("app_main initialization-order test passed");
    return 0;
}
