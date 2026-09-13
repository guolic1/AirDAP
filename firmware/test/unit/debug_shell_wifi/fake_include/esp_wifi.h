#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { bool show_hidden; } wifi_scan_config_t;
typedef struct { uint8_t ssid[33], bssid[6], primary; int8_t rssi; unsigned authmode; } wifi_ap_record_t;
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records);
esp_err_t esp_wifi_clear_ap_list(void);
