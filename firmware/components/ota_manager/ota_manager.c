#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_device_identity.h"
#include "airdap_ota.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

typedef enum {
    OTA_SESSION_IDLE,
    OTA_SESSION_RECEIVING,
    OTA_SESSION_COMMITTED,
} ota_session_state_t;

typedef struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    uint32_t expected_size;
    uint32_t written_size;
    ota_session_state_t state;
} ota_session_t;

static ota_session_t session;

static void reset_session(void)
{
    memset(&session, 0, sizeof(session));
    session.state = OTA_SESSION_IDLE;
}

void airdap_ota_initialize(void)
{
    if (session.state == OTA_SESSION_RECEIVING) {
        (void) esp_ota_abort(session.handle);
    }
    reset_session();
}

bool airdap_ota_debug_allowed(void)
{
    return session.state == OTA_SESSION_IDLE;
}

airdap_ota_status_t airdap_ota_get_info(airdap_ota_info_t *info)
{
    if (info == NULL) {
        return AIRDAP_OTA_STATUS_INVALID_ARGUMENT;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    const airdap_device_identity_t *identity = airdap_device_identity_get();
    if (partition == NULL || identity == NULL || partition->size > UINT32_MAX) {
        return AIRDAP_OTA_STATUS_INTERNAL_ERROR;
    }

    memset(info, 0, sizeof(*info));
    info->protocol_version = AIRDAP_OTA_PROTOCOL_VERSION;
    info->flags = AIRDAP_OTA_FLAG_ROLLBACK;
    info->max_image_size = (uint32_t) partition->size;
    size_t version_length = 0U;
    while (version_length < AIRDAP_OTA_VERSION_CAPACITY &&
           identity->firmware_version[version_length] != '\0') {
        ++version_length;
    }
    memcpy(info->running_version, identity->firmware_version, version_length);
    info->running_version[version_length] = '\0';
    return AIRDAP_OTA_STATUS_OK;
}

airdap_ota_status_t airdap_ota_begin(uint32_t image_size)
{
    if (session.state != OTA_SESSION_IDLE) {
        return AIRDAP_OTA_STATUS_INVALID_STATE;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || partition->size > UINT32_MAX) {
        return AIRDAP_OTA_STATUS_INTERNAL_ERROR;
    }
    if (image_size == 0U || image_size > partition->size) {
        return AIRDAP_OTA_STATUS_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0U;
    if (esp_ota_begin(partition, image_size, &handle) != ESP_OK) {
        if (handle != 0U) {
            (void) esp_ota_abort(handle);
        }
        return AIRDAP_OTA_STATUS_INTERNAL_ERROR;
    }

    session.partition = partition;
    session.handle = handle;
    session.expected_size = image_size;
    session.written_size = 0U;
    session.state = OTA_SESSION_RECEIVING;
    return AIRDAP_OTA_STATUS_OK;
}

airdap_ota_status_t airdap_ota_write(
    uint32_t offset,
    const void *data,
    size_t size,
    uint32_t *next_offset)
{
    if (session.state != OTA_SESSION_RECEIVING) {
        return AIRDAP_OTA_STATUS_INVALID_STATE;
    }
    if (data == NULL || next_offset == NULL) {
        return AIRDAP_OTA_STATUS_INVALID_ARGUMENT;
    }
    if (offset != session.written_size) {
        return AIRDAP_OTA_STATUS_INVALID_OFFSET;
    }
    if (size == 0U || size > session.expected_size - session.written_size) {
        return AIRDAP_OTA_STATUS_INVALID_SIZE;
    }

    if (esp_ota_write(session.handle, data, size) != ESP_OK) {
        (void) esp_ota_abort(session.handle);
        reset_session();
        return AIRDAP_OTA_STATUS_WRITE_FAILED;
    }

    session.written_size += (uint32_t) size;
    *next_offset = session.written_size;
    return AIRDAP_OTA_STATUS_OK;
}

airdap_ota_status_t airdap_ota_commit(void)
{
    if (session.state != OTA_SESSION_RECEIVING) {
        return AIRDAP_OTA_STATUS_INVALID_STATE;
    }
    if (session.written_size != session.expected_size) {
        return AIRDAP_OTA_STATUS_INCOMPLETE_IMAGE;
    }

    const esp_partition_t *partition = session.partition;
    const esp_ota_handle_t handle = session.handle;
    if (esp_ota_end(handle) != ESP_OK) {
        reset_session();
        return AIRDAP_OTA_STATUS_VALIDATION_FAILED;
    }
    if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        reset_session();
        return AIRDAP_OTA_STATUS_ACTIVATION_FAILED;
    }

    session.partition = partition;
    session.handle = 0U;
    session.expected_size = 0U;
    session.written_size = 0U;
    session.state = OTA_SESSION_COMMITTED;
    return AIRDAP_OTA_STATUS_OK;
}

airdap_ota_status_t airdap_ota_abort(void)
{
    if (session.state == OTA_SESSION_IDLE) {
        return AIRDAP_OTA_STATUS_OK;
    }
    if (session.state == OTA_SESSION_COMMITTED) {
        return AIRDAP_OTA_STATUS_INVALID_STATE;
    }

    const esp_err_t error = esp_ota_abort(session.handle);
    reset_session();
    return error == ESP_OK
        ? AIRDAP_OTA_STATUS_OK
        : AIRDAP_OTA_STATUS_INTERNAL_ERROR;
}

void airdap_ota_handle_disconnect(void)
{
    if (session.state == OTA_SESSION_RECEIVING) {
        (void) airdap_ota_abort();
    }
}

airdap_ota_status_t airdap_ota_reboot(void)
{
    if (session.state != OTA_SESSION_COMMITTED) {
        return AIRDAP_OTA_STATUS_INVALID_STATE;
    }
    esp_restart();
    return AIRDAP_OTA_STATUS_OK;
}

esp_err_t airdap_ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_FAIL;
    }

    esp_ota_img_states_t state;
    const esp_err_t error = esp_ota_get_state_partition(running, &state);
    if (error != ESP_OK) {
        return error;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}
