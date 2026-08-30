#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "airdap_config_store.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char tag[] = "config_store_hil";
static const char fixture_name[] = "AirDAP HIL fixture";
static const uint8_t wifi_fixture[] = {0x10, 0x20, 0x30};
static const uint8_t pairing_fixture[] = {0x40, 0x50};
static const uint8_t auth_fixture[] = {0x60, 0x70, 0x80, 0x90};

static void require(bool condition, const char *message)
{
    if (!condition) {
        ESP_LOGE(tag, "FAIL: %s", message);
        abort();
    }
}

static bool blob_matches(
    airdap_config_slot_t slot,
    const uint8_t *expected,
    size_t expected_size)
{
    uint8_t output[16];
    size_t output_size = sizeof(output);
    const esp_err_t error = airdap_config_store_get_blob(
        slot,
        output,
        &output_size);
    return error == ESP_OK && output_size == expected_size &&
        memcmp(output, expected, expected_size) == 0;
}

static void wait_for_power_cycle(void)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(airdap_config_store_init());

    char friendly_name[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE];
    ESP_ERROR_CHECK(airdap_config_store_get_friendly_name(
        friendly_name,
        sizeof(friendly_name)));

    if (friendly_name[0] == '\0') {
        ESP_ERROR_CHECK(airdap_config_store_set_friendly_name(fixture_name));
        ESP_ERROR_CHECK(airdap_config_store_set_blob(
            AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
            wifi_fixture,
            sizeof(wifi_fixture)));
        ESP_ERROR_CHECK(airdap_config_store_set_blob(
            AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
            pairing_fixture,
            sizeof(pairing_fixture)));
        ESP_ERROR_CHECK(airdap_config_store_set_blob(
            AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
            auth_fixture,
            sizeof(auth_fixture)));
        ESP_ERROR_CHECK(airdap_config_store_set_provisioned(true));
        ESP_LOGW(tag, "STAGE_WRITE_OK: remove board power now");
        wait_for_power_cycle();
    }

    require(strcmp(friendly_name, fixture_name) == 0,
        "unexpected friendly name");
    airdap_config_status_t status;
    ESP_ERROR_CHECK(airdap_config_store_get_status(&status));

    if (status.provisioned) {
        require(blob_matches(
            AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
            wifi_fixture,
            sizeof(wifi_fixture)),
            "Wi-Fi fixture was not restored");
        require(blob_matches(
            AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
            pairing_fixture,
            sizeof(pairing_fixture)),
            "pairing fixture was not restored");
        require(blob_matches(
            AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
            auth_fixture,
            sizeof(auth_fixture)),
            "auth fixture was not restored");
        ESP_LOGI(tag, "STAGE_RESTORE_OK: committed fields survived power loss");

        ESP_ERROR_CHECK(airdap_config_store_clear(
            AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS |
            AIRDAP_CONFIG_CLEAR_PAIRING_RECORD));
        ESP_LOGW(tag, "STAGE_CLEAR_OK: remove board power now");
        wait_for_power_cycle();
    }

    size_t cleared_size = 0U;
    require(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
        NULL,
        &cleared_size) == ESP_ERR_NOT_FOUND,
        "Wi-Fi fixture survived selective clear");
    require(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
        NULL,
        &cleared_size) == ESP_ERR_NOT_FOUND,
        "pairing fixture survived selective clear");
    require(blob_matches(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        auth_fixture,
        sizeof(auth_fixture)),
        "auth fixture was removed by selective clear");
    ESP_ERROR_CHECK(airdap_config_store_get_status(&status));
    require(!status.provisioned, "selective clear did not reset provisioning");
    ESP_LOGI(
        tag,
        "PASS: restore and selective clear survived hard power cycles");
    wait_for_power_cycle();
}
