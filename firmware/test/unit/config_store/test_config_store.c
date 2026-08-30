#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "airdap_config_store.h"
#include "airdap_config_store_internal.h"
#include "esp_err.h"
#include "freertos/semphr.h"
#include "nvs.h"

enum {
    FAKE_HANDLE = 1,
};

static airdap_config_store_record_t durable_record;
static airdap_config_store_record_t pending_record;
static bool durable_present;
static bool pending_present;
static bool handle_open;
static nvs_open_mode_t last_open_mode;
static esp_err_t flash_init_once;
static esp_err_t erase_result;
static esp_err_t open_result;
static esp_err_t get_result;
static esp_err_t set_result;
static esp_err_t commit_result;
static unsigned flash_init_calls;
static unsigned erase_calls;
static unsigned open_calls;
static unsigned close_calls;
static unsigned set_calls;
static unsigned commit_calls;
static pthread_mutex_t interleave_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interleave_condition = PTHREAD_COND_INITIALIZER;
static unsigned mutex_take_attempts;
static bool block_next_commit;
static bool blocked_commit_entered;
static bool release_blocked_commit;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *mutex_buffer)
{
    assert(mutex_buffer != NULL);
    if (mutex_buffer->initialized) {
        assert(pthread_mutex_destroy(&mutex_buffer->mutex) == 0);
    }
    assert(pthread_mutex_init(&mutex_buffer->mutex, NULL) == 0);
    mutex_buffer->initialized = true;
    return mutex_buffer;
}

BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t timeout_ticks)
{
    assert(semaphore != NULL);
    assert(timeout_ticks == portMAX_DELAY);
    assert(pthread_mutex_lock(&interleave_mutex) == 0);
    ++mutex_take_attempts;
    assert(pthread_cond_broadcast(&interleave_condition) == 0);
    assert(pthread_mutex_unlock(&interleave_mutex) == 0);
    return pthread_mutex_lock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL);
    return pthread_mutex_unlock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

static void reset_fake_storage(void)
{
    memset(&durable_record, 0, sizeof(durable_record));
    memset(&pending_record, 0, sizeof(pending_record));
    durable_present = false;
    pending_present = false;
    handle_open = false;
    last_open_mode = NVS_READONLY;
    flash_init_once = ESP_OK;
    erase_result = ESP_OK;
    open_result = ESP_OK;
    get_result = ESP_OK;
    set_result = ESP_OK;
    commit_result = ESP_OK;
    flash_init_calls = 0U;
    erase_calls = 0U;
    open_calls = 0U;
    close_calls = 0U;
    set_calls = 0U;
    commit_calls = 0U;
    mutex_take_attempts = 0U;
    block_next_commit = false;
    blocked_commit_entered = false;
    release_blocked_commit = false;
}

esp_err_t nvs_flash_init(void)
{
    ++flash_init_calls;
    const esp_err_t result = flash_init_once;
    flash_init_once = ESP_OK;
    return result;
}

esp_err_t nvs_flash_erase(void)
{
    ++erase_calls;
    if (erase_result == ESP_OK) {
        durable_present = false;
        memset(&durable_record, 0, sizeof(durable_record));
    }
    return erase_result;
}

esp_err_t nvs_open(
    const char *namespace_name,
    nvs_open_mode_t open_mode,
    nvs_handle_t *out_handle)
{
    assert(strcmp(namespace_name, "airdap") == 0);
    assert(out_handle != NULL);
    ++open_calls;
    if (open_result != ESP_OK) {
        return open_result;
    }
    assert(!handle_open);
    handle_open = true;
    last_open_mode = open_mode;
    pending_record = durable_record;
    pending_present = durable_present;
    *out_handle = FAKE_HANDLE;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == FAKE_HANDLE);
    assert(handle_open);
    handle_open = false;
    ++close_calls;
}

esp_err_t nvs_set_blob(
    nvs_handle_t handle,
    const char *key,
    const void *value,
    size_t length)
{
    assert(handle == FAKE_HANDLE && handle_open);
    assert(last_open_mode == NVS_READWRITE_PURGE);
    assert(strcmp(key, "config") == 0);
    assert(value != NULL);
    ++set_calls;
    if (set_result != ESP_OK) {
        return set_result;
    }
    assert(length == sizeof(pending_record));
    memcpy(&pending_record, value, length);
    pending_present = true;
    return ESP_OK;
}

esp_err_t nvs_get_blob(
    nvs_handle_t handle,
    const char *key,
    void *out_value,
    size_t *length)
{
    assert(handle == FAKE_HANDLE && handle_open);
    assert(strcmp(key, "config") == 0);
    assert(length != NULL);
    if (get_result != ESP_OK) {
        return get_result;
    }
    if (!durable_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value == NULL) {
        *length = sizeof(durable_record);
        return ESP_OK;
    }
    if (*length < sizeof(durable_record)) {
        *length = sizeof(durable_record);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, &durable_record, sizeof(durable_record));
    *length = sizeof(durable_record);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == FAKE_HANDLE && handle_open);
    ++commit_calls;
    assert(pthread_mutex_lock(&interleave_mutex) == 0);
    if (block_next_commit) {
        block_next_commit = false;
        blocked_commit_entered = true;
        assert(pthread_cond_broadcast(&interleave_condition) == 0);
        while (!release_blocked_commit) {
            assert(pthread_cond_wait(
                &interleave_condition,
                &interleave_mutex) == 0);
        }
    }
    assert(pthread_mutex_unlock(&interleave_mutex) == 0);
    if (commit_result != ESP_OK) {
        return commit_result;
    }
    durable_record = pending_record;
    durable_present = pending_present;
    return ESP_OK;
}

static void assert_default_record(const airdap_config_store_record_t *record)
{
    assert(record->magic == AIRDAP_CONFIG_STORE_RECORD_MAGIC);
    assert(record->schema_version == AIRDAP_CONFIG_SCHEMA_VERSION);
    assert(record->record_size == sizeof(*record));
    assert(record->provisioned == 0U);
    assert(record->friendly_name[0] == '\0');
    for (size_t index = 0U; index < AIRDAP_CONFIG_SLOT_COUNT; ++index) {
        assert(record->slots[index].length == 0U);
    }
}

static void test_initialization_error_and_recovery_boundaries(void)
{
    reset_fake_storage();

    airdap_config_status_t status;
    assert(airdap_config_store_get_status(&status) == ESP_ERR_INVALID_STATE);
    assert(airdap_config_store_set_friendly_name("not ready") ==
        ESP_ERR_INVALID_STATE);
    assert(airdap_config_store_load_record(NULL) == ESP_ERR_INVALID_ARG);

    flash_init_once = ESP_FAIL;
    assert(airdap_config_store_init() == ESP_FAIL);
    assert(erase_calls == 0U);

    durable_present = true;
    durable_record.magic = AIRDAP_CONFIG_STORE_RECORD_MAGIC;
    durable_record.schema_version = AIRDAP_CONFIG_SCHEMA_VERSION + 1U;
    durable_record.record_size = sizeof(durable_record);
    assert(airdap_config_store_init() == ESP_ERR_INVALID_VERSION);
    assert(erase_calls == 0U);
    assert(durable_record.schema_version == AIRDAP_CONFIG_SCHEMA_VERSION + 1U);
    assert(open_calls == close_calls);

    flash_init_once = ESP_ERR_NVS_NO_FREE_PAGES;
    erase_result = ESP_FAIL;
    assert(airdap_config_store_init() == ESP_FAIL);
    assert(erase_calls == 1U);

    flash_init_once = ESP_ERR_NVS_NEW_VERSION_FOUND;
    erase_result = ESP_OK;
    assert(airdap_config_store_init() == ESP_OK);
    assert(erase_calls == 2U);
    assert(flash_init_calls == 5U);
    assert(last_open_mode == NVS_READWRITE_PURGE);
    assert(set_calls == 1U && commit_calls == 1U);
    assert_default_record(&durable_record);
    assert(open_calls == close_calls);
    assert(airdap_config_store_init() == ESP_ERR_INVALID_STATE);
}

static void test_committed_configuration_is_readable(void)
{
    static const uint8_t wifi_credentials[] = {0x10, 0x20, 0x30};
    static const uint8_t pairing_record[] = {0x41, 0x42, 0x43, 0x44};
    static const uint8_t auth_material[] = {0xA5, 0x5A};
    char friendly_name[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE];
    airdap_config_status_t status;

    assert(airdap_config_store_set_friendly_name("Lab AirDAP") == ESP_OK);
    assert(airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
        wifi_credentials,
        sizeof(wifi_credentials)) == ESP_OK);
    assert(airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
        pairing_record,
        sizeof(pairing_record)) == ESP_OK);
    assert(airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        auth_material,
        sizeof(auth_material)) == ESP_OK);
    assert(airdap_config_store_set_provisioned(true) == ESP_OK);

    assert(airdap_config_store_get_status(&status) == ESP_OK);
    assert(status.schema_version == AIRDAP_CONFIG_SCHEMA_VERSION);
    assert(status.provisioned);
    assert(airdap_config_store_get_friendly_name(
        friendly_name,
        sizeof(friendly_name)) == ESP_OK);
    assert(strcmp(friendly_name, "Lab AirDAP") == 0);

    size_t required = 0U;
    assert(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
        NULL,
        &required) == ESP_OK);
    assert(required == sizeof(pairing_record));
    uint8_t output[sizeof(pairing_record)];
    size_t output_size = sizeof(output);
    assert(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
        output,
        &output_size) == ESP_OK);
    assert(output_size == sizeof(pairing_record));
    assert(memcmp(output, pairing_record, sizeof(output)) == 0);

    airdap_config_store_record_t loaded;
    assert(airdap_config_store_load_record(&loaded) == ESP_OK);
    assert(strcmp(loaded.friendly_name, "Lab AirDAP") == 0);
    assert(loaded.provisioned == 1U);
    assert(loaded.slots[AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS].length ==
        sizeof(wifi_credentials));
    assert(memcmp(
        loaded.slots[AIRDAP_CONFIG_SLOT_AUTH_MATERIAL].data,
        auth_material,
        sizeof(auth_material)) == 0);
}

static void test_failed_writes_do_not_publish_or_commit_candidates(void)
{
    char friendly_name[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE];
    const airdap_config_store_record_t before = durable_record;

    set_result = ESP_FAIL;
    assert(airdap_config_store_set_friendly_name("set failed") == ESP_FAIL);
    set_result = ESP_OK;
    assert(memcmp(&durable_record, &before, sizeof(before)) == 0);
    assert(airdap_config_store_get_friendly_name(
        friendly_name,
        sizeof(friendly_name)) == ESP_OK);
    assert(strcmp(friendly_name, "Lab AirDAP") == 0);

    commit_result = ESP_FAIL;
    assert(airdap_config_store_set_friendly_name("commit failed") == ESP_FAIL);
    commit_result = ESP_OK;
    assert(memcmp(&durable_record, &before, sizeof(before)) == 0);
    assert(airdap_config_store_get_friendly_name(
        friendly_name,
        sizeof(friendly_name)) == ESP_OK);
    assert(strcmp(friendly_name, "Lab AirDAP") == 0);
    assert(open_calls == close_calls);
}

static void test_selective_clear_preserves_unselected_fields(void)
{
    const unsigned commits_before = commit_calls;
    assert(airdap_config_store_clear(
        AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS |
        AIRDAP_CONFIG_CLEAR_PAIRING_RECORD) == ESP_OK);
    assert(commit_calls == commits_before + 1U);

    airdap_config_status_t status;
    assert(airdap_config_store_get_status(&status) == ESP_OK);
    assert(!status.provisioned);

    char friendly_name[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE];
    assert(airdap_config_store_get_friendly_name(
        friendly_name,
        sizeof(friendly_name)) == ESP_OK);
    assert(strcmp(friendly_name, "Lab AirDAP") == 0);

    size_t size = 0U;
    assert(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
        NULL,
        &size) == ESP_ERR_NOT_FOUND);
    assert(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_PAIRING_RECORD,
        NULL,
        &size) == ESP_ERR_NOT_FOUND);
    assert(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        NULL,
        &size) == ESP_OK);
    assert(size == 2U);
}

typedef struct {
    esp_err_t result;
} concurrent_write_result_t;

static void *write_friendly_name(void *context)
{
    concurrent_write_result_t *result = context;
    result->result = airdap_config_store_set_friendly_name("Concurrent AirDAP");
    return NULL;
}

static void *write_provisioning_state(void *context)
{
    concurrent_write_result_t *result = context;
    result->result = airdap_config_store_set_provisioned(true);
    return NULL;
}

static void test_concurrent_writes_are_serialized(void)
{
    assert(airdap_config_store_set_friendly_name("Before concurrent") == ESP_OK);
    assert(airdap_config_store_set_provisioned(false) == ESP_OK);

    assert(pthread_mutex_lock(&interleave_mutex) == 0);
    const unsigned take_attempts_before = mutex_take_attempts;
    block_next_commit = true;
    blocked_commit_entered = false;
    release_blocked_commit = false;
    assert(pthread_mutex_unlock(&interleave_mutex) == 0);

    concurrent_write_result_t friendly_result = {.result = ESP_FAIL};
    concurrent_write_result_t provisioning_result = {.result = ESP_FAIL};
    pthread_t friendly_thread;
    pthread_t provisioning_thread;
    assert(pthread_create(
        &friendly_thread,
        NULL,
        write_friendly_name,
        &friendly_result) == 0);

    assert(pthread_mutex_lock(&interleave_mutex) == 0);
    while (!blocked_commit_entered) {
        assert(pthread_cond_wait(
            &interleave_condition,
            &interleave_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&interleave_mutex) == 0);

    assert(pthread_create(
        &provisioning_thread,
        NULL,
        write_provisioning_state,
        &provisioning_result) == 0);

    assert(pthread_mutex_lock(&interleave_mutex) == 0);
    while (mutex_take_attempts < take_attempts_before + 2U) {
        assert(pthread_cond_wait(
            &interleave_condition,
            &interleave_mutex) == 0);
    }
    release_blocked_commit = true;
    assert(pthread_cond_broadcast(&interleave_condition) == 0);
    assert(pthread_mutex_unlock(&interleave_mutex) == 0);

    assert(pthread_join(friendly_thread, NULL) == 0);
    assert(pthread_join(provisioning_thread, NULL) == 0);
    assert(friendly_result.result == ESP_OK);
    assert(provisioning_result.result == ESP_OK);

    char friendly_name[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE];
    airdap_config_status_t status;
    assert(airdap_config_store_get_friendly_name(
        friendly_name,
        sizeof(friendly_name)) == ESP_OK);
    assert(strcmp(friendly_name, "Concurrent AirDAP") == 0);
    assert(airdap_config_store_get_status(&status) == ESP_OK);
    assert(status.provisioned);
    assert(strcmp(durable_record.friendly_name, "Concurrent AirDAP") == 0);
    assert(durable_record.provisioned == 1U);
}

static void test_public_api_restores_committed_state_after_reboot(void)
{
    const unsigned commits_before = commit_calls;
    airdap_config_store_reset_for_test();
    assert(airdap_config_store_init() == ESP_OK);
    assert(commit_calls == commits_before);

    char friendly_name[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE];
    airdap_config_status_t status;
    assert(airdap_config_store_get_friendly_name(
        friendly_name,
        sizeof(friendly_name)) == ESP_OK);
    assert(strcmp(friendly_name, "Concurrent AirDAP") == 0);
    assert(airdap_config_store_get_status(&status) == ESP_OK);
    assert(status.schema_version == AIRDAP_CONFIG_SCHEMA_VERSION);
    assert(status.provisioned);

    uint8_t auth_material[2];
    size_t auth_material_size = sizeof(auth_material);
    assert(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        auth_material,
        &auth_material_size) == ESP_OK);
    assert(auth_material_size == sizeof(auth_material));
    assert(auth_material[0] == 0xA5 && auth_material[1] == 0x5A);
}

static void test_read_and_record_errors_remain_observable(void)
{
    char short_name[4];
    assert(airdap_config_store_get_friendly_name(
        short_name,
        sizeof(short_name)) == ESP_ERR_INVALID_SIZE);

    uint8_t short_blob[1];
    size_t short_size = sizeof(short_blob);
    assert(airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        short_blob,
        &short_size) == ESP_ERR_INVALID_SIZE);
    assert(short_size == 2U);

    const airdap_config_store_record_t saved = durable_record;
    airdap_config_store_record_t loaded;

    get_result = ESP_FAIL;
    assert(airdap_config_store_load_record(&loaded) == ESP_FAIL);
    get_result = ESP_OK;
    assert(!handle_open);

    durable_record.magic ^= 1U;
    assert(airdap_config_store_load_record(&loaded) == ESP_ERR_INVALID_RESPONSE);
    durable_record = saved;

    durable_record.schema_version = AIRDAP_CONFIG_SCHEMA_VERSION + 1U;
    assert(airdap_config_store_load_record(&loaded) == ESP_ERR_INVALID_VERSION);
    durable_record = saved;

    durable_record.record_size = sizeof(durable_record) - 1U;
    assert(airdap_config_store_load_record(&loaded) == ESP_ERR_INVALID_SIZE);
    durable_record = saved;

    durable_record.provisioned = 2U;
    assert(airdap_config_store_load_record(&loaded) == ESP_ERR_INVALID_RESPONSE);
    durable_record = saved;

    memset(durable_record.friendly_name, 'X',
        sizeof(durable_record.friendly_name));
    assert(airdap_config_store_load_record(&loaded) == ESP_ERR_INVALID_RESPONSE);
    durable_record = saved;

    durable_record.slots[AIRDAP_CONFIG_SLOT_AUTH_MATERIAL].length =
        AIRDAP_CONFIG_BLOB_MAX_SIZE + 1U;
    assert(airdap_config_store_load_record(&loaded) == ESP_ERR_INVALID_SIZE);
    durable_record = saved;

    open_result = ESP_FAIL;
    assert(airdap_config_store_load_record(&loaded) == ESP_FAIL);
    open_result = ESP_OK;
    assert(!handle_open);
}

static void test_rejects_invalid_arguments_and_lengths(void)
{
    char too_long[AIRDAP_CONFIG_FRIENDLY_NAME_SIZE + 1U];
    memset(too_long, 'A', sizeof(too_long));
    too_long[sizeof(too_long) - 1U] = '\0';
    assert(airdap_config_store_set_friendly_name(NULL) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_set_friendly_name(too_long) == ESP_ERR_INVALID_SIZE);
    assert(airdap_config_store_get_friendly_name(NULL, 1U) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_get_friendly_name(too_long, 0U) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_get_status(NULL) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_COUNT,
        too_long,
        1U) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        NULL,
        1U) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        too_long,
        0U) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        too_long,
        AIRDAP_CONFIG_BLOB_MAX_SIZE + 1U) == ESP_ERR_INVALID_SIZE);
    assert(airdap_config_store_clear(0U) == ESP_ERR_INVALID_ARG);
    assert(airdap_config_store_clear(1U << 31U) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_initialization_error_and_recovery_boundaries();
    test_committed_configuration_is_readable();
    test_failed_writes_do_not_publish_or_commit_candidates();
    test_selective_clear_preserves_unselected_fields();
    test_concurrent_writes_are_serialized();
    test_public_api_restores_committed_state_after_reboot();
    test_read_and_record_errors_remain_observable();
    test_rejects_invalid_arguments_and_lengths();

    puts("Config store tests passed");
    return 0;
}
