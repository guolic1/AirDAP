#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_WIFI_SSID_MAX_LENGTH = 32,
    AIRDAP_WIFI_PASSWORD_MAX_LENGTH = 64,
};

typedef struct {
    uint8_t ssid[AIRDAP_WIFI_SSID_MAX_LENGTH];
    uint8_t password[AIRDAP_WIFI_PASSWORD_MAX_LENGTH];
    uint8_t ssid_length;
    uint8_t password_length;
} airdap_wifi_credentials_t;

esp_err_t airdap_wifi_manager_start(void);
esp_err_t airdap_wifi_manager_set_credentials(
    const airdap_wifi_credentials_t *credentials);
esp_err_t airdap_wifi_manager_clear_credentials(void);

#ifdef __cplusplus
}
#endif
