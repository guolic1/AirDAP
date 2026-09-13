#include <stdio.h>
#include <stdlib.h>
#include "airdap_debug_shell_wifi_scan.h"
#include "airdap_wifi_manager.h"
#include "esp_wifi.h"

esp_err_t airdap_debug_shell_wifi_scan(void **records, uint16_t *count)
{
    *records = NULL;
    *count = 0;
    airdap_wifi_manager_info_t info;
    esp_err_t error = airdap_wifi_manager_get_info(&info);
    if (error != ESP_OK) return error;
    if (!info.radio_enabled || info.provisioning_suspended) return ESP_ERR_INVALID_STATE;
    const wifi_scan_config_t config = {.show_hidden = true};
    error = esp_wifi_scan_start(&config, true);
    if (error != ESP_OK) return error;
    error = esp_wifi_scan_get_ap_num(count);
    if (error != ESP_OK || *count > 1024) {
        (void) esp_wifi_clear_ap_list();
        *count = 0;
        return error != ESP_OK ? error : ESP_ERR_NO_MEM;
    }
    if (*count == 0) {
        (void) esp_wifi_clear_ap_list();
        return ESP_OK;
    }
    wifi_ap_record_t *snapshot = calloc(*count, sizeof(*snapshot));
    if (snapshot == NULL) {
        (void) esp_wifi_clear_ap_list();
        *count = 0;
        return ESP_ERR_NO_MEM;
    }
    error = esp_wifi_scan_get_ap_records(count, snapshot);
    if (error != ESP_OK) {
        free(snapshot);
        *count = 0;
        (void) esp_wifi_clear_ap_list();
        return error;
    }
    *records = snapshot;
    return ESP_OK;
}

bool airdap_debug_shell_wifi_scan_format(const void *records, unsigned index,
    char *output, size_t output_size)
{
    const wifi_ap_record_t *ap = (const wifi_ap_record_t *) records + index;
    char ssid[65] = {0};
    for (size_t i = 0; i < 32 && ap->ssid[i] != 0; ++i)
        (void) snprintf(ssid + i * 2, 3, "%02x", ap->ssid[i]);
    const int size = snprintf(output, output_size,
        "ap=%s,%02x%02x%02x%02x%02x%02x,%u,%d,%u\n", ssid,
        ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5],
        (unsigned) ap->primary, (int) ap->rssi, (unsigned) ap->authmode);
    return size >= 0 && (size_t) size < output_size;
}
