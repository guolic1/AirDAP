#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "airdap_button_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

enum { RECORD_VERSION = 1, RECORD_SIZE = 1 + AIRDAP_BUTTON_GESTURE_COUNT };
static const unsigned int INITIALIZED = 1U << 31;
static atomic_uint bindings;
static atomic_bool storage_ready;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static const uint8_t defaults[AIRDAP_BUTTON_GESTURE_COUNT] = {
    AIRDAP_BUTTON_COMMAND_NONE,
    AIRDAP_BUTTON_COMMAND_DAP_TOGGLE,
    AIRDAP_BUTTON_COMMAND_PROVISIONING,
    AIRDAP_BUTTON_COMMAND_NONE,
    AIRDAP_BUTTON_COMMAND_CLEAR_NETWORK_RESTART,
};
static const char *const command_names[] = {
    "none", "dap-toggle", "provisioning", "clear-network-restart",
    "dap-usb", "dap-network", "dap-auto",
};
static const char *const gesture_names[] = {"single", "double", "hold2", "hold6", "hold10"};
_Static_assert(AIRDAP_BUTTON_COMMAND_COUNT <= 16, "bindings use four bits per command");
_Static_assert(AIRDAP_BUTTON_GESTURE_COUNT * 4 < 31, "bindings must fit atomic word");

const char *airdap_button_command_name(airdap_button_command_t command)
{
    return command >= 0 && command < AIRDAP_BUTTON_COMMAND_COUNT ? command_names[command] : NULL;
}

const char *airdap_button_gesture_name(airdap_button_gesture_t gesture)
{
    return gesture >= 0 && gesture < AIRDAP_BUTTON_GESTURE_COUNT ? gesture_names[gesture] : NULL;
}

static unsigned int pack(const uint8_t *commands)
{
    unsigned int word = INITIALIZED;
    for (unsigned i = 0; i < AIRDAP_BUTTON_GESTURE_COUNT; ++i) {
        word |= (unsigned int) commands[i] << (4U * i);
    }
    return word;
}

esp_err_t airdap_button_config_get(uint8_t commands[AIRDAP_BUTTON_GESTURE_COUNT])
{
    if (commands == NULL) return ESP_ERR_INVALID_ARG;
    const unsigned int word = atomic_load(&bindings);
    if ((word & INITIALIZED) == 0U) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < AIRDAP_BUTTON_GESTURE_COUNT; ++i) {
        commands[i] = (uint8_t) ((word >> (4U * i)) & 0xFU);
    }
    return ESP_OK;
}

static esp_err_t initialization_result(esp_err_t error)
{
    /* Publish the mutex only after startup I/O has finished. The USB shell may
     * already be running, including when a bad record needs explicit recovery. */
    atomic_store(&storage_ready, true);
    return error;
}

esp_err_t airdap_button_config_init(void)
{
    if ((atomic_load(&bindings) & INITIALIZED) != 0U) return ESP_ERR_INVALID_STATE;
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    if (mutex == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("airdap_btn", NVS_READWRITE, &handle);
    if (error != ESP_OK) return initialization_result(error);
    uint8_t record[RECORD_SIZE] = {RECORD_VERSION};
    size_t size = sizeof(record);
    error = nvs_get_blob(handle, "bindings", record, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        memcpy(record + 1, defaults, sizeof(defaults));
        error = ESP_OK;
    } else if (error == ESP_OK) {
        if (size != sizeof(record)) error = ESP_ERR_INVALID_SIZE;
        else if (record[0] != RECORD_VERSION) error = ESP_ERR_INVALID_VERSION;
    }
    if (error != ESP_OK) return initialization_result(error);
    for (unsigned i = 1; i < sizeof(record); ++i) {
        if (record[i] >= AIRDAP_BUTTON_COMMAND_COUNT) return initialization_result(ESP_ERR_INVALID_ARG);
    }
    atomic_store(&bindings, pack(record + 1));
    return initialization_result(ESP_OK);
}

static esp_err_t update(int gesture, airdap_button_command_t command)
{
    if (!atomic_load(&storage_ready) || (gesture >= 0 && (atomic_load(&bindings) & INITIALIZED) == 0U)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    uint8_t record[RECORD_SIZE] = {RECORD_VERSION};
    esp_err_t error = gesture < 0 ? ESP_OK : airdap_button_config_get(record + 1);
    if (error == ESP_OK) {
        if (gesture < 0) memcpy(record + 1, defaults, sizeof(defaults));
        else record[1 + gesture] = (uint8_t) command;
        if (pack(record + 1) != atomic_load(&bindings)) {
            nvs_handle_t handle;
            error = nvs_open("airdap_btn", NVS_READWRITE, &handle);
            if (error == ESP_OK) {
                error = nvs_set_blob(handle, "bindings", record, sizeof(record));
                if (error == ESP_OK) error = nvs_commit(handle);
                nvs_close(handle);
            }
            if (error == ESP_OK) atomic_store(&bindings, pack(record + 1));
        }
    }
    (void) xSemaphoreGive(mutex);
    return error;
}

esp_err_t airdap_button_config_set(airdap_button_gesture_t gesture, airdap_button_command_t command)
{
    if (airdap_button_gesture_name(gesture) == NULL || airdap_button_command_name(command) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return update((int) gesture, command);
}

esp_err_t airdap_button_config_defaults(void)
{
    return update(-1, AIRDAP_BUTTON_COMMAND_NONE);
}

#ifdef AIRDAP_BUTTON_CONFIG_TESTING
void airdap_button_config_test_reset(void)
{
    atomic_store(&bindings, 0U);
    atomic_store(&storage_ready, false);
}
#endif
