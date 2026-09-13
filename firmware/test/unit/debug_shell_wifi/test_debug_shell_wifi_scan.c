#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "airdap_debug_shell_wifi_scan.h"
#include "airdap_wifi_manager.h"
#include "esp_wifi.h"

static bool radio = true, provisioning;
static unsigned scan_calls, cleared;
static uint16_t available = 2;
static esp_err_t fetch_result;
esp_err_t airdap_wifi_manager_get_info(airdap_wifi_manager_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->radio_enabled = radio;
    info->provisioning_suspended = provisioning;
    return ESP_OK;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking)
{ assert(config->show_hidden && blocking); ++scan_calls; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) { *count = available; return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list(void) { ++cleared; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{
    assert(*count == 2);
    memcpy(records[0].ssid, "Example", 7);
    records[0].rssi = -42; records[0].primary = 6; records[0].authmode = 3;
    records[1].rssi = -70;
    return fetch_result;
}
int main(void)
{
    void *records = NULL; uint16_t count = 0; char output[128];
    assert(airdap_debug_shell_wifi_scan(&records, &count) == ESP_OK && count == 2);
    assert(airdap_debug_shell_wifi_scan_format(records, 0, output, sizeof(output)));
    assert(strcmp(output, "ap=4578616d706c65,000000000000,6,-42,3\n") == 0);
    assert(airdap_debug_shell_wifi_scan_format(records, 1, output, sizeof(output)));
    assert(strncmp(output, "ap=,", 4) == 0);
    assert(!airdap_debug_shell_wifi_scan_format(records, 0, output, 4));
    free(records);
    radio = false;
    assert(airdap_debug_shell_wifi_scan(&records, &count) == ESP_ERR_INVALID_STATE);
    radio = true; provisioning = true;
    assert(airdap_debug_shell_wifi_scan(&records, &count) == ESP_ERR_INVALID_STATE);
    assert(scan_calls == 1);
    provisioning = false; fetch_result = ESP_FAIL;
    assert(airdap_debug_shell_wifi_scan(&records, &count) == ESP_FAIL);
    assert(records == NULL && count == 0 && cleared == 1);
    available = 0;
    assert(airdap_debug_shell_wifi_scan(&records, &count) == ESP_OK && records == NULL);
    available = 1025;
    assert(airdap_debug_shell_wifi_scan(&records, &count) == ESP_ERR_NO_MEM);
    assert(records == NULL && count == 0 && cleared == 3);
    return 0;
}
