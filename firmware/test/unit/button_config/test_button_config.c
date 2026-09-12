#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "airdap_button_config.h"
#include "airdap_debug_shell_button.h"
#include "airdap_mode_state.h"
#include "freertos/semphr.h"
#include "nvs.h"

void airdap_button_config_test_reset(void);
static uint8_t durable[32], pending[32];
static size_t durable_size, pending_size;
static bool handle_open;
static bool verify_initialization_guard = true;
static esp_err_t open_error, set_error, commit_error;
static unsigned writes, commits, input_changes;
static char output[2048];

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    if (buffer->initialized) assert(pthread_mutex_destroy(&buffer->mutex) == 0);
    assert(pthread_mutex_init(&buffer->mutex, NULL) == 0);
    buffer->initialized = true;
    return buffer;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks)
{
    assert(ticks == portMAX_DELAY);
    return pthread_mutex_lock(&mutex->mutex) == 0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    return pthread_mutex_unlock(&mutex->mutex) == 0 ? pdTRUE : pdFALSE;
}
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "airdap_btn") == 0 && mode == NVS_READWRITE);
    if (open_error != ESP_OK) return open_error;
    assert(!handle_open);
    handle_open = true;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1 && handle_open);
    handle_open = false;
    pending_size = 0;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size)
{
    assert(handle == 1 && handle_open && strcmp(key, "bindings") == 0);
    if (verify_initialization_guard) {
        assert(airdap_button_config_defaults() == ESP_ERR_INVALID_STATE);
    }
    if (durable_size == 0) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < durable_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(data, durable, durable_size);
    *size = durable_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && handle_open && strcmp(key, "bindings") == 0 && size == 6);
    ++writes;
    if (set_error != ESP_OK) return set_error;
    memcpy(pending, data, size);
    pending_size = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && handle_open && pending_size == 6);
    ++commits;
    if (commit_error != ESP_OK) return commit_error;
    memcpy(durable, pending, pending_size);
    durable_size = pending_size;
    return ESP_OK;
}
esp_err_t airdap_boot_key_set_simulated_pressed(bool pressed)
{
    (void) pressed;
    ++input_changes;
    return ESP_OK;
}
esp_err_t airdap_boot_key_get_simulated_pressed(bool *pressed)
{
    *pressed = false;
    return ESP_OK;
}
airdap_dap_route_t airdap_mode_state_get_dap_route(void) { return AIRDAP_DAP_ROUTE_AUTO; }
static void capture(airdap_debug_shell_style_t style, const char *format, va_list args, void *context)
{
    (void) style;
    (void) context;
    size_t used = strlen(output);
    vsnprintf(output + used, sizeof(output) - used, format, args);
}
static int shell(const char *text)
{
    output[0] = '\0';
    const airdap_debug_shell_invocation_t invocation = {.vprintf = capture};
    return airdap_debug_shell_button_command(text, &invocation, NULL);
}
static void *set_binding(void *argument)
{
    const airdap_button_gesture_t gesture = *(const airdap_button_gesture_t *) argument;
    assert(airdap_button_config_set(gesture, AIRDAP_BUTTON_COMMAND_DAP_USB) == ESP_OK);
    return NULL;
}
int main(void)
{
    uint8_t commands[5];
    assert(airdap_button_config_get(commands) == ESP_ERR_INVALID_STATE);
    assert(airdap_button_config_defaults() == ESP_ERR_INVALID_STATE);
    assert(airdap_button_config_init() == ESP_OK);
    verify_initialization_guard = false;
    assert(airdap_button_config_get(commands) == ESP_OK);
    const uint8_t expected[5] = {0, 1, 2, 0, 3};
    assert(memcmp(commands, expected, 5) == 0 && writes == 0);
    assert(shell("commands") == 0);
    assert(strcmp(output, "none\ndap-toggle\nprovisioning\nclear-network-restart\ndap-usb\ndap-network\ndap-auto\nrestart\ntarget-reset\nwifi-toggle\ntarget-power-toggle\ntarget-power-cycle\n") == 0);
    assert(shell("bindings") == 0 && strstr(output, "hold6=none") && strstr(output, "dap-route=auto"));
    const char *invalid[] = {"bind single invalid", "bind hold3 none", "bind hold6 none extra", "bind hold6", "commands extra"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(shell(invalid[i]) == 1);
    assert(writes == 0 && input_changes == 0);
    assert(shell("bind hold6 dap-usb") == 0 && strstr(output, "saved"));
    airdap_button_config_test_reset();
    assert(airdap_button_config_init() == ESP_OK);
    assert(airdap_button_config_get(commands) == ESP_OK && commands[3] == AIRDAP_BUTTON_COMMAND_DAP_USB);
    const unsigned saved_writes = writes;
    assert(shell("bind hold6 dap-usb") == 0 && writes == saved_writes);
    commit_error = ESP_FAIL;
    assert(shell("bind hold6 none") == 1 && strstr(output, "save failed"));
    assert(airdap_button_config_get(commands) == ESP_OK && commands[3] == AIRDAP_BUTTON_COMMAND_DAP_USB);
    commit_error = ESP_OK;
    set_error = ESP_FAIL;
    assert(shell("defaults") == 1);
    set_error = ESP_OK;
    open_error = ESP_FAIL;
    assert(shell("defaults") == 1);
    open_error = ESP_OK;
    assert(shell("defaults") == 0);
    assert(airdap_button_config_get(commands) == ESP_OK && memcmp(commands, expected, 5) == 0);
    for (int g = 0; g < AIRDAP_BUTTON_GESTURE_COUNT; ++g) {
        for (int c = 0; c < AIRDAP_BUTTON_COMMAND_COUNT; ++c) {
            assert(airdap_button_config_set((airdap_button_gesture_t) g, (airdap_button_command_t) c) == ESP_OK);
            assert(airdap_button_config_get(commands) == ESP_OK && commands[g] == c);
        }
    }
    assert(airdap_button_config_defaults() == ESP_OK);
    pthread_t first, second;
    assert(shell("bind hold6 target-power-cycle") == 0);
    airdap_button_config_test_reset();
    assert(airdap_button_config_init() == ESP_OK);
    assert(airdap_button_config_get(commands) == ESP_OK && commands[3] == AIRDAP_BUTTON_COMMAND_TARGET_POWER_CYCLE);
    airdap_button_gesture_t g1 = AIRDAP_BUTTON_GESTURE_SINGLE, g2 = AIRDAP_BUTTON_GESTURE_HOLD6;
    assert(pthread_create(&first, NULL, set_binding, &g1) == 0);
    assert(pthread_create(&second, NULL, set_binding, &g2) == 0);
    assert(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
    airdap_button_config_test_reset();
    assert(airdap_button_config_init() == ESP_OK);
    assert(airdap_button_config_get(commands) == ESP_OK && commands[0] == 4 && commands[3] == 4);
    assert(airdap_button_config_get(NULL) == ESP_ERR_INVALID_ARG);
    assert(airdap_button_config_set((airdap_button_gesture_t) 5, AIRDAP_BUTTON_COMMAND_NONE) == ESP_ERR_INVALID_ARG);
    assert(airdap_button_config_set(g1, AIRDAP_BUTTON_COMMAND_COUNT) == ESP_ERR_INVALID_ARG);
    airdap_button_config_test_reset();
    durable[0] = 99;
    assert(airdap_button_config_init() == ESP_ERR_INVALID_VERSION);
    durable[0] = 1;
    durable[1] = 99;
    assert(airdap_button_config_init() == ESP_ERR_INVALID_ARG);
    durable_size = 2;
    assert(airdap_button_config_init() == ESP_ERR_INVALID_SIZE);
    assert(shell("bindings") == 1);
    assert(shell("defaults") == 0);
    airdap_button_config_test_reset();
    assert(airdap_button_config_init() == ESP_OK);
    assert(airdap_button_config_get(commands) == ESP_OK && memcmp(commands, expected, 5) == 0);
    assert(input_changes == 0 && commits > 0);
    puts("Button persistence and shell tests passed");
    return 0;
}
