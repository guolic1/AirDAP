#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS = 0,
} airdap_config_slot_t;

enum {
    AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS = 1U << 1,
    AIRDAP_CONFIG_CLEAR_PAIRING_RECORD = 1U << 2,
    AIRDAP_CONFIG_CLEAR_AUTH_MATERIAL = 1U << 3,
    AIRDAP_CONFIG_CLEAR_NETWORK =
        AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS |
        AIRDAP_CONFIG_CLEAR_PAIRING_RECORD |
        AIRDAP_CONFIG_CLEAR_AUTH_MATERIAL,
};

esp_err_t airdap_config_store_get_blob(
    airdap_config_slot_t slot,
    void *output,
    size_t *inout_size);
esp_err_t airdap_config_store_set_blob(
    airdap_config_slot_t slot,
    const void *data,
    size_t data_size);
esp_err_t airdap_config_store_commit_network_provisioning(
    const void *wifi_credentials,
    size_t wifi_credentials_size);
esp_err_t airdap_config_store_clear(uint32_t flags);
