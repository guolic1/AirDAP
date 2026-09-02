#include <inttypes.h>

#include "sdkconfig.h"

#include "airdap_board.h"
#include "airdap_config_store.h"
#include "airdap_device_identity.h"
#include "airdap_discovery.h"
#include "airdap_mode_state.h"
#include "airdap_ota.h"
#include "airdap_swd.h"
#include "airdap_usb.h"
#include "airdap_voltage_monitor.h"
#include "airdap_wifi_manager.h"
#include "esp_err.h"
#include "esp_log.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "AirDAP firmware requires an ESP32-S3 target"
#endif

static const char *TAG = "airdap";

void app_main(void)
{
    airdap_voltage_reading_t voltage;

    airdap_mode_state_init();
    ESP_ERROR_CHECK(airdap_ota_initialize());
    ESP_ERROR_CHECK(airdap_board_init_safe());
    ESP_ERROR_CHECK(airdap_device_identity_init());
    ESP_ERROR_CHECK(airdap_config_store_init());
    ESP_ERROR_CHECK(airdap_voltage_monitor_init());
    ESP_ERROR_CHECK(airdap_swd_init(AIRDAP_SWD_DEFAULT_CLOCK_HZ));
    ESP_ERROR_CHECK(airdap_voltage_monitor_read(&voltage));
    ESP_ERROR_CHECK(airdap_usb_init());
    ESP_ERROR_CHECK(airdap_ota_confirm_running_image());

    const esp_err_t wifi_error = airdap_wifi_manager_start();
    if (wifi_error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_error));
    } else {
        const esp_err_t discovery_error = airdap_discovery_start();
        if (discovery_error != ESP_OK) {
            ESP_LOGW(TAG, "mDNS discovery unavailable: %s",
                esp_err_to_name(discovery_error));
        }
    }

    ESP_LOGI(
        TAG,
        "AirDAP firmware started: target=%" PRIu32 " mV, USB VBUS=%" PRIu32 " mV",
        voltage.target_mv,
        voltage.usb_vbus_mv);
}
