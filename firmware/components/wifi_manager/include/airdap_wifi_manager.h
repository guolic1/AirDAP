#pragma once

#include <stdbool.h>
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

typedef enum {
    AIRDAP_WIFI_MANAGER_FAILURE_NONE = 0,
    AIRDAP_WIFI_MANAGER_FAILURE_AUTHENTICATION,
    AIRDAP_WIFI_MANAGER_FAILURE_TRANSIENT,
} airdap_wifi_manager_failure_t;

typedef struct {
    bool started;
    bool has_configuration;
    bool link_connected;
    bool provisioning_suspended;
    airdap_wifi_manager_failure_t last_failure;
    uint8_t last_disconnect_reason;
    uint32_t retry_delay_ms;
    bool retry_scheduled;
    bool ipv4_available;
    uint8_t ipv4_address[4];
    uint8_t ipv4_netmask[4];
    uint8_t ipv4_gateway[4];
    bool ap_available;
    int8_t rssi_dbm;
    uint8_t channel;
} airdap_wifi_manager_info_t;

esp_err_t airdap_wifi_manager_start(void);
esp_err_t airdap_wifi_manager_get_info(airdap_wifi_manager_info_t *info);
esp_err_t airdap_wifi_manager_set_credentials(
    const airdap_wifi_credentials_t *credentials);
esp_err_t airdap_wifi_manager_clear_credentials(void);

/* Provisioning-manager integration. prepare suspends AirDAP's connection
 * controller before the upstream manager takes ownership. stage validates the
 * candidate that the upstream manager holds in RAM. finish resumes the AirDAP
 * controller after the upstream manager has ended and synchronously reapplies
 * the canonical config_store value. prepare, stage, accept, and finish run on
 * the default event loop. Wi-Fi driver NVS persistence must remain disabled. */
esp_err_t airdap_wifi_manager_stage_provisioning_credentials(
    const airdap_wifi_credentials_t *credentials);
esp_err_t airdap_wifi_manager_prepare_provisioning(void);
esp_err_t airdap_wifi_manager_accept_provisioned_credentials(
    const airdap_wifi_credentials_t *credentials);
esp_err_t airdap_wifi_manager_finish_provisioning(void);
esp_err_t airdap_wifi_manager_clear_network_configuration(void);

#ifdef __cplusplus
}
#endif
