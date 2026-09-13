#include <stdint.h>

#include "airdap_mode_storage.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "airdap_mode";

bool airdap_mode_storage_load(airdap_dap_route_t *route)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("airdap_mode", NVS_READONLY, &handle);
    uint8_t value = AIRDAP_DAP_ROUTE_AUTO;
    if (error == ESP_OK) {
        error = nvs_get_u8(handle, "dap_route", &value);
        nvs_close(handle);
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        value = AIRDAP_DAP_ROUTE_AUTO;
        error = ESP_OK;
    }
    if (error == ESP_OK && value > AIRDAP_DAP_ROUTE_NETWORK) error = ESP_ERR_INVALID_ARG;
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Read DAP route failed: %s", esp_err_to_name(error));
        return false;
    }
    *route = (airdap_dap_route_t) value;
    return true;
}

bool airdap_mode_storage_save(airdap_dap_route_t route)
{
    if (route < AIRDAP_DAP_ROUTE_AUTO || route > AIRDAP_DAP_ROUTE_NETWORK) return false;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("airdap_mode", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_u8(handle, "dap_route", (uint8_t) route);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) ESP_LOGE(TAG, "Save DAP route failed: %s", esp_err_to_name(error));
    return error == ESP_OK;
}
