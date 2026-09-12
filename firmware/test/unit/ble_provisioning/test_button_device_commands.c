#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "airdap_board.h"
#include "airdap_board_pins.h"
#include "airdap_button_device_commands.h"
#include "airdap_mode_state.h"
#include "airdap_wifi_manager.h"
#include "driver/gpio.h"
#include "esp_timer.h"

struct fake_esp_timer { void (*callback)(void *); void *arg; bool active; uint64_t us; };
static struct fake_esp_timer timer;
static esp_err_t create_error, timer_error, gpio_error, wifi_error;
static unsigned gpio_calls, line_resets, restarts, wifi_toggles;
static bool key_pressed;
static bool release_pins(void *context) { (void) context; return true; }
static bool line_reset(void *context) { (void) context; ++line_resets; return true; }
esp_err_t gpio_config(const gpio_config_t *config) { (void) config; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    (void) pin; (void) level;
    ++gpio_calls;
    return gpio_error;
}
int gpio_get_level(gpio_num_t pin) { return pin == AIRDAP_PIN_BOOT_KEY ? !key_pressed : 0; }
esp_err_t airdap_wifi_manager_toggle(void) { ++wifi_toggles; return wifi_error; }
void esp_restart(void) { ++restarts; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle)
{
    if (create_error != ESP_OK) return create_error;
    timer.callback = args->callback; timer.arg = args->arg; *handle = &timer;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t us)
{
    assert(handle == &timer && !timer.active);
    if (timer_error != ESP_OK) return timer_error;
    timer.us = us; timer.active = true; return ESP_OK;
}
static void expire(void)
{
    assert(timer.active);
    timer.active = false;
    timer.callback(timer.arg);
}
static esp_err_t run(airdap_button_command_t command)
{
    return airdap_button_device_command_execute(command);
}
int main(void)
{
    const airdap_dap_ownership_backend_t backend = {.line_reset = line_reset, .release_pins = release_pins};
    assert(airdap_dap_ownership_initialize(&backend) == AIRDAP_DAP_OWNERSHIP_OK);
    airdap_mode_state_init();
    assert(airdap_board_init_safe() == ESP_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED) == AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_DEBUG_SHELL_STARTED) == AIRDAP_MODE_STATE_OK);
    create_error = ESP_FAIL;
    unsigned calls = gpio_calls;
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_RESET) == ESP_FAIL && gpio_calls == calls);
    create_error = ESP_OK;
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_RESET) == ESP_OK);
    assert(airdap_target_reset_is_asserted() && timer.us == 100000 && timer.active);
    assert(airdap_button_device_command_busy());
    assert(run(AIRDAP_BUTTON_COMMAND_RESTART) == ESP_ERR_INVALID_STATE && restarts == 0);
    airdap_dap_ownership_claim_t claim;
    assert(airdap_mode_state_dap_acquire(AIRDAP_DAP_OWNER_USB, false, &claim) == AIRDAP_MODE_DAP_BUSY);
    assert(airdap_dap_ownership_suspend() == AIRDAP_DAP_OWNERSHIP_BUSY);
    expire();
    assert(!airdap_target_reset_is_asserted() && !airdap_button_device_command_busy());
    assert(line_resets == 0);
    assert(airdap_target_power_is_allowed());
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_POWER_TOGGLE) == ESP_OK && !airdap_target_power_is_allowed());
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_POWER_TOGGLE) == ESP_OK && airdap_target_power_is_allowed());
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_POWER_CYCLE) == ESP_OK);
    assert(!airdap_target_power_is_allowed() && timer.us == 500000);
    assert(run(AIRDAP_BUTTON_COMMAND_WIFI_TOGGLE) == ESP_ERR_INVALID_STATE && wifi_toggles == 0);
    expire();
    assert(airdap_target_power_is_allowed());
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_POWER_CYCLE) == ESP_OK);
    gpio_error = ESP_FAIL;
    expire();
    assert(!airdap_target_power_is_allowed() && !airdap_button_device_command_busy());
    gpio_error = ESP_OK;
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_POWER_TOGGLE) == ESP_OK);
    timer_error = ESP_FAIL;
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_RESET) == ESP_FAIL);
    assert(!airdap_target_reset_is_asserted() && !airdap_button_device_command_busy());
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_POWER_CYCLE) == ESP_FAIL);
    assert(airdap_target_power_is_allowed());
    timer_error = ESP_OK;
    gpio_error = ESP_FAIL;
    assert(run(AIRDAP_BUTTON_COMMAND_TARGET_POWER_TOGGLE) == ESP_FAIL);
    assert(airdap_target_power_is_allowed() && !timer.active);
    gpio_error = ESP_OK;
    assert(run(AIRDAP_BUTTON_COMMAND_WIFI_TOGGLE) == ESP_OK && wifi_toggles == 1);
    wifi_error = ESP_FAIL;
    assert(run(AIRDAP_BUTTON_COMMAND_WIFI_TOGGLE) == ESP_FAIL && wifi_toggles == 2);
    key_pressed = true;
    assert(run(AIRDAP_BUTTON_COMMAND_RESTART) == ESP_ERR_INVALID_STATE && restarts == 0);
    key_pressed = false;
    assert(run(AIRDAP_BUTTON_COMMAND_RESTART) == ESP_OK && restarts == 1);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_STARTED) == AIRDAP_MODE_STATE_OK);
    for (int c = AIRDAP_BUTTON_COMMAND_RESTART; c < AIRDAP_BUTTON_COMMAND_COUNT; ++c) {
        assert(run((airdap_button_command_t) c) == ESP_ERR_INVALID_STATE);
    }
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_ABORTED) == AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_dap_acquire(AIRDAP_DAP_OWNER_USB, false, &claim) == AIRDAP_MODE_DAP_ALLOWED);
    calls = gpio_calls;
    for (int c = AIRDAP_BUTTON_COMMAND_RESTART; c < AIRDAP_BUTTON_COMMAND_COUNT; ++c) {
        assert(run((airdap_button_command_t) c) == ESP_ERR_INVALID_STATE);
    }
    assert(gpio_calls == calls && restarts == 1 && wifi_toggles == 2);
    assert(airdap_dap_ownership_release(&claim) == AIRDAP_DAP_OWNERSHIP_OK);
    assert(!airdap_button_device_command_busy());
    puts("Button device commands passed");
    return 0;
}
