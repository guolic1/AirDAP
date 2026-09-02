#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_CONFIG_SCHEMA_VERSION = 1,
    AIRDAP_CONFIG_FRIENDLY_NAME_MAX_LENGTH = 32,
    AIRDAP_CONFIG_FRIENDLY_NAME_SIZE =
        AIRDAP_CONFIG_FRIENDLY_NAME_MAX_LENGTH + 1,
    AIRDAP_CONFIG_BLOB_MAX_SIZE = 1024,
};

typedef enum {
    AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
    AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
    AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
    AIRDAP_CONFIG_SLOT_COUNT,
} airdap_config_slot_t;

typedef uint32_t airdap_config_clear_flags_t;

enum {
    AIRDAP_CONFIG_CLEAR_FRIENDLY_NAME = 1U << 0,
    AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS = 1U << 1,
    AIRDAP_CONFIG_CLEAR_PAIRING_RECORD = 1U << 2,
    AIRDAP_CONFIG_CLEAR_AUTH_MATERIAL = 1U << 3,
    AIRDAP_CONFIG_CLEAR_NETWORK =
        AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS |
        AIRDAP_CONFIG_CLEAR_PAIRING_RECORD |
        AIRDAP_CONFIG_CLEAR_AUTH_MATERIAL,
    AIRDAP_CONFIG_CLEAR_ALL =
        AIRDAP_CONFIG_CLEAR_FRIENDLY_NAME |
        AIRDAP_CONFIG_CLEAR_NETWORK,
};

typedef struct {
    uint32_t schema_version;
    bool provisioned;
} airdap_config_status_t;

esp_err_t airdap_config_store_init(void);
esp_err_t airdap_config_store_get_status(airdap_config_status_t *status);
esp_err_t airdap_config_store_get_friendly_name(
    char *friendly_name,
    size_t friendly_name_size);
esp_err_t airdap_config_store_set_friendly_name(const char *friendly_name);
esp_err_t airdap_config_store_set_provisioned(bool provisioned);
esp_err_t airdap_config_store_get_blob(
    airdap_config_slot_t slot,
    void *output,
    size_t *inout_size);
esp_err_t airdap_config_store_set_blob(
    airdap_config_slot_t slot,
    const void *data,
    size_t data_size);
/* Atomically publishes successful Wi-Fi provisioning with its credential blob. */
esp_err_t airdap_config_store_commit_network_provisioning(
    const void *wifi_credentials,
    size_t wifi_credentials_size);
esp_err_t airdap_config_store_clear(airdap_config_clear_flags_t flags);

#ifdef __cplusplus
}
#endif
