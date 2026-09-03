#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_config_store.h"
#include "airdap_config_store_internal.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char config_namespace[] = "airdap";
static const char config_key[] = "config";

static airdap_config_store_record_t current_record;
static airdap_config_store_record_t candidate_record;
static StaticSemaphore_t config_mutex_storage;
static SemaphoreHandle_t config_mutex;
static bool initialized;

_Static_assert(
    sizeof(airdap_config_store_record_t) <= UINT32_MAX,
    "config record size must fit its persistent size field");

static void make_default_record(airdap_config_store_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = AIRDAP_CONFIG_STORE_RECORD_MAGIC;
    record->schema_version = AIRDAP_CONFIG_SCHEMA_VERSION;
    record->record_size = (uint32_t) sizeof(*record);
}

static void clear_candidate_record(void)
{
    volatile uint8_t *byte = (volatile uint8_t *) &candidate_record;
    for (size_t index = 0U; index < sizeof(candidate_record); ++index) {
        byte[index] = 0U;
    }
}

static esp_err_t validate_record(const airdap_config_store_record_t *record)
{
    if (record->magic != AIRDAP_CONFIG_STORE_RECORD_MAGIC) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (record->schema_version != AIRDAP_CONFIG_SCHEMA_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (record->record_size != (uint32_t) sizeof(*record)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->provisioned > 1U ||
        memchr(record->friendly_name, '\0', sizeof(record->friendly_name)) == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t index = 0U; index < AIRDAP_CONFIG_SLOT_COUNT; ++index) {
        if (record->slots[index].length > AIRDAP_CONFIG_BLOB_MAX_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

static esp_err_t persist_record(
    nvs_handle_t handle,
    const airdap_config_store_record_t *record)
{
    esp_err_t error = nvs_set_blob(handle, config_key, record, sizeof(*record));
    if (error != ESP_OK) {
        return error;
    }
    return nvs_commit(handle);
}

esp_err_t airdap_config_store_load_record(
    airdap_config_store_record_t *record)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(
        config_namespace,
        NVS_READWRITE_PURGE,
        &handle);
    if (error != ESP_OK) {
        return error;
    }

    size_t stored_size = 0U;
    error = nvs_get_blob(handle, config_key, NULL, &stored_size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        make_default_record(record);
        error = persist_record(handle, record);
    } else if (error == ESP_OK && stored_size != sizeof(*record)) {
        error = ESP_ERR_INVALID_SIZE;
    } else if (error == ESP_OK) {
        stored_size = sizeof(*record);
        error = nvs_get_blob(handle, config_key, record, &stored_size);
        if (error == ESP_OK && stored_size != sizeof(*record)) {
            error = ESP_ERR_INVALID_SIZE;
        }
        if (error == ESP_OK) {
            error = validate_record(record);
        }
    }

    nvs_close(handle);
    return error;
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error != ESP_ERR_NVS_NO_FREE_PAGES &&
        error != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return error;
    }

    error = nvs_flash_erase();
    if (error != ESP_OK) {
        return error;
    }
    return nvs_flash_init();
}

esp_err_t airdap_config_store_init(void)
{
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = initialize_nvs();
    if (error != ESP_OK) {
        return error;
    }

    error = airdap_config_store_load_record(&current_record);
    if (error != ESP_OK) {
        return error;
    }

    config_mutex = xSemaphoreCreateMutexStatic(&config_mutex_storage);
    if (config_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    initialized = true;
    return ESP_OK;
}

static esp_err_t lock_config_store(void)
{
    if (!initialized || config_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(config_mutex, portMAX_DELAY) == pdTRUE
        ? ESP_OK
        : ESP_FAIL;
}

static void unlock_config_store(void)
{
    (void) xSemaphoreGive(config_mutex);
}

static esp_err_t update_record(const airdap_config_store_record_t *candidate)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(
        config_namespace,
        NVS_READWRITE_PURGE,
        &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = persist_record(handle, candidate);
    nvs_close(handle);
    if (error == ESP_OK) {
        current_record = *candidate;
    }
    return error;
}

static size_t bounded_string_length(const char *value, size_t limit)
{
    size_t length = 0U;
    while (length < limit && value[length] != '\0') {
        ++length;
    }
    return length;
}

esp_err_t airdap_config_store_get_status(airdap_config_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }
    status->schema_version = current_record.schema_version;
    status->provisioned = current_record.provisioned != 0U;
    unlock_config_store();
    return ESP_OK;
}

esp_err_t airdap_config_store_get_friendly_name(
    char *friendly_name,
    size_t friendly_name_size)
{
    if (friendly_name == NULL || friendly_name_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }
    const size_t required = strlen(current_record.friendly_name) + 1U;
    if (friendly_name_size < required) {
        unlock_config_store();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(friendly_name, current_record.friendly_name, required);
    unlock_config_store();
    return ESP_OK;
}

esp_err_t airdap_config_store_set_friendly_name(const char *friendly_name)
{
    if (friendly_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = bounded_string_length(
        friendly_name,
        AIRDAP_CONFIG_FRIENDLY_NAME_SIZE);
    if (length >= AIRDAP_CONFIG_FRIENDLY_NAME_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }
    if (strcmp(current_record.friendly_name, friendly_name) == 0) {
        unlock_config_store();
        return ESP_OK;
    }

    candidate_record = current_record;
    memset(
        candidate_record.friendly_name,
        0,
        sizeof(candidate_record.friendly_name));
    memcpy(candidate_record.friendly_name, friendly_name, length);
    error = update_record(&candidate_record);
    clear_candidate_record();
    unlock_config_store();
    return error;
}

esp_err_t airdap_config_store_set_provisioned(bool provisioned)
{
    esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }
    if ((current_record.provisioned != 0U) == provisioned) {
        unlock_config_store();
        return ESP_OK;
    }

    candidate_record = current_record;
    candidate_record.provisioned = provisioned ? 1U : 0U;
    error = update_record(&candidate_record);
    clear_candidate_record();
    unlock_config_store();
    return error;
}

static bool slot_is_valid(airdap_config_slot_t slot)
{
    return slot >= AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS &&
        slot < AIRDAP_CONFIG_SLOT_COUNT;
}

esp_err_t airdap_config_store_get_blob(
    airdap_config_slot_t slot,
    void *output,
    size_t *inout_size)
{
    if (!slot_is_valid(slot) || inout_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }

    const airdap_config_store_blob_t *blob = &current_record.slots[slot];
    if (blob->length == 0U) {
        *inout_size = 0U;
        unlock_config_store();
        return ESP_ERR_NOT_FOUND;
    }
    if (output == NULL) {
        *inout_size = blob->length;
        unlock_config_store();
        return ESP_OK;
    }
    if (*inout_size < blob->length) {
        *inout_size = blob->length;
        unlock_config_store();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(output, blob->data, blob->length);
    *inout_size = blob->length;
    unlock_config_store();
    return ESP_OK;
}

esp_err_t airdap_config_store_set_blob(
    airdap_config_slot_t slot,
    const void *data,
    size_t data_size)
{
    if (!slot_is_valid(slot) || data == NULL || data_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_size > AIRDAP_CONFIG_BLOB_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }

    const airdap_config_store_blob_t *current = &current_record.slots[slot];
    if (current->length == data_size &&
        memcmp(current->data, data, data_size) == 0) {
        unlock_config_store();
        return ESP_OK;
    }

    candidate_record = current_record;
    airdap_config_store_blob_t *blob = &candidate_record.slots[slot];
    memset(blob, 0, sizeof(*blob));
    memcpy(blob->data, data, data_size);
    blob->length = (uint32_t) data_size;
    error = update_record(&candidate_record);
    clear_candidate_record();
    unlock_config_store();
    return error;
}

esp_err_t airdap_config_store_commit_network_provisioning(
    const void *wifi_credentials,
    size_t wifi_credentials_size)
{
    if (wifi_credentials == NULL || wifi_credentials_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (wifi_credentials_size > AIRDAP_CONFIG_BLOB_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }

    const airdap_config_store_blob_t *current =
        &current_record.slots[AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS];
    if (current_record.provisioned != 0U &&
        current->length == wifi_credentials_size &&
        memcmp(current->data, wifi_credentials, wifi_credentials_size) == 0) {
        unlock_config_store();
        return ESP_OK;
    }

    candidate_record = current_record;
    airdap_config_store_blob_t *blob =
        &candidate_record.slots[AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS];
    memset(blob, 0, sizeof(*blob));
    memcpy(blob->data, wifi_credentials, wifi_credentials_size);
    blob->length = (uint32_t) wifi_credentials_size;
    candidate_record.provisioned = 1U;
    error = update_record(&candidate_record);
    clear_candidate_record();
    unlock_config_store();
    return error;
}

static airdap_config_clear_flags_t slot_clear_flag(
    airdap_config_slot_t slot)
{
    switch (slot) {
    case AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS:
        return AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS;
    case AIRDAP_CONFIG_SLOT_PAIRING_RECORD:
        return AIRDAP_CONFIG_CLEAR_PAIRING_RECORD;
    case AIRDAP_CONFIG_SLOT_AUTH_MATERIAL:
        return AIRDAP_CONFIG_CLEAR_AUTH_MATERIAL;
    case AIRDAP_CONFIG_SLOT_COUNT:
        break;
    }
    return 0U;
}

esp_err_t airdap_config_store_clear(airdap_config_clear_flags_t flags)
{
    if (flags == 0U || (flags & ~AIRDAP_CONFIG_CLEAR_ALL) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = lock_config_store();
    if (error != ESP_OK) {
        return error;
    }

    candidate_record = current_record;
    if ((flags & AIRDAP_CONFIG_CLEAR_FRIENDLY_NAME) != 0U) {
        memset(
            candidate_record.friendly_name,
            0,
            sizeof(candidate_record.friendly_name));
    }
    for (airdap_config_slot_t slot = AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS;
         slot < AIRDAP_CONFIG_SLOT_COUNT;
         slot = (airdap_config_slot_t) (slot + 1)) {
        if ((flags & slot_clear_flag(slot)) != 0U) {
            memset(
                &candidate_record.slots[slot],
                0,
                sizeof(candidate_record.slots[slot]));
        }
    }
    if ((flags & AIRDAP_CONFIG_CLEAR_NETWORK) != 0U) {
        candidate_record.provisioned = 0U;
    }
    if (memcmp(
            &candidate_record,
            &current_record,
            sizeof(candidate_record)) == 0) {
        clear_candidate_record();
        unlock_config_store();
        return ESP_OK;
    }
    error = update_record(&candidate_record);
    clear_candidate_record();
    unlock_config_store();
    return error;
}

#ifdef AIRDAP_CONFIG_STORE_TESTING
void airdap_config_store_reset_for_test(void)
{
    initialized = false;
    config_mutex = NULL;
    memset(&current_record, 0, sizeof(current_record));
    clear_candidate_record();
}
#endif
