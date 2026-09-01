#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_OTA_PROTOCOL_VERSION = 1,
    AIRDAP_OTA_FLAG_ROLLBACK = 1U << 0,
    AIRDAP_OTA_VERSION_CAPACITY = 32,
};

typedef enum {
    AIRDAP_OTA_STATUS_OK = 0,
    AIRDAP_OTA_STATUS_INVALID_ARGUMENT = 1,
    AIRDAP_OTA_STATUS_INVALID_STATE = 2,
    AIRDAP_OTA_STATUS_INVALID_SIZE = 3,
    AIRDAP_OTA_STATUS_INVALID_OFFSET = 4,
    AIRDAP_OTA_STATUS_INCOMPLETE_IMAGE = 5,
    AIRDAP_OTA_STATUS_WRITE_FAILED = 6,
    AIRDAP_OTA_STATUS_VALIDATION_FAILED = 7,
    AIRDAP_OTA_STATUS_ACTIVATION_FAILED = 8,
    AIRDAP_OTA_STATUS_INTERNAL_ERROR = 9,
} airdap_ota_status_t;

typedef struct {
    uint32_t max_image_size;
    uint8_t protocol_version;
    uint8_t flags;
    char running_version[AIRDAP_OTA_VERSION_CAPACITY + 1U];
} airdap_ota_info_t;

esp_err_t airdap_ota_initialize(void);
bool airdap_ota_debug_allowed(void);

airdap_ota_status_t airdap_ota_get_info(airdap_ota_info_t *info);
airdap_ota_status_t airdap_ota_begin(uint32_t image_size);
airdap_ota_status_t airdap_ota_write(
    uint32_t offset,
    const void *data,
    size_t size,
    uint32_t *next_offset);
airdap_ota_status_t airdap_ota_commit(void);
airdap_ota_status_t airdap_ota_abort(void);
airdap_ota_status_t airdap_ota_reboot(void);

void airdap_ota_handle_disconnect(void);
esp_err_t airdap_ota_confirm_running_image(void);

#ifdef __cplusplus
}
#endif
