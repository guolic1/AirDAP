#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

enum {
    WIFI_EVENT_STA_START = 0,
    WIFI_EVENT_STA_CONNECTED,
    WIFI_EVENT_STA_DISCONNECTED,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
    WIFI_REASON_802_1X_AUTH_FAILED = 23,
    WIFI_REASON_BEACON_TIMEOUT = 200,
    WIFI_REASON_NO_AP_FOUND = 201,
    WIFI_REASON_AUTH_FAIL = 202,
    WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
    WIFI_REASON_UNSPECIFIED = 1,
    WIFI_IF_STA = 0,
    WIFI_STORAGE_RAM = 1,
    WIFI_STORAGE_FLASH = 2,
    WIFI_MODE_STA = 1,
    WIFI_ALL_CHANNEL_SCAN = 1,
    WIFI_CONNECT_AP_BY_SIGNAL = 1,
};

extern esp_event_base_t WIFI_EVENT;

typedef struct {
    uint8_t reason;
} wifi_event_sta_disconnected_t;

typedef struct {
    int unused;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t) {0})

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    int scan_method;
    int sort_method;
    uint8_t failure_retry_cnt;
    struct {
        bool capable;
        bool required;
    } pmf_cfg;
} wifi_sta_config_t;

typedef struct {
    wifi_sta_config_t sta;
} wifi_config_t;

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
