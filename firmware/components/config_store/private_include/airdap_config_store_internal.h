#pragma once

#include <stdint.h>

#include "airdap_config_store.h"

enum {
    AIRDAP_CONFIG_STORE_RECORD_MAGIC = 0x43464441,
};

typedef struct {
    uint32_t length;
    uint8_t data[AIRDAP_CONFIG_BLOB_MAX_SIZE];
} airdap_config_store_blob_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    uint32_t record_size;
    char friendly_name[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE];
    uint8_t provisioned;
    uint8_t reserved[2];
    airdap_config_store_blob_t slots[AIRDAP_CONFIG_SLOT_COUNT];
} airdap_config_store_record_t;

esp_err_t airdap_config_store_load_record(
    airdap_config_store_record_t *record);

#ifdef AIRDAP_CONFIG_STORE_TESTING
void airdap_config_store_reset_for_test(void);
#endif
