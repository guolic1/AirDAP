#include <stdatomic.h>
#include <stddef.h>
#include "airdap_board.h"
#include "airdap_button_device_commands.h"
#include "airdap_mode_state.h"
#include "airdap_wifi_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "airdap_button";
static atomic_bool busy;
static esp_timer_handle_t pulse_timer;
static airdap_dap_ownership_operation_t pulse_operation;
static bool pulse_is_reset;

bool airdap_button_device_command_busy(void) { return atomic_load(&busy); }

static void finish_pulse(void *argument)
{
    (void) argument;
    const esp_err_t error = pulse_is_reset ? airdap_target_reset_set_asserted(false)
        : airdap_target_power_set_allowed(true);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Button pulse restore failed: %s", esp_err_to_name(error));
    } else {
        ESP_LOGI(TAG, "Button pulse completed (%s)", pulse_is_reset ? "reset" : "power");
    }
    airdap_dap_ownership_operation_end(&pulse_operation);
    atomic_store(&busy, false);
}

esp_err_t airdap_button_device_command_execute(airdap_button_command_t command)
{
    if (command < AIRDAP_BUTTON_COMMAND_RESTART || command >= AIRDAP_BUTTON_COMMAND_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) return ESP_ERR_INVALID_STATE;
    airdap_dap_ownership_operation_t operation = {0};
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (airdap_dap_ownership_control_begin(AIRDAP_DAP_OWNER_DIAGNOSTIC, &operation) !=
        AIRDAP_DAP_OWNERSHIP_OK) goto done;
    airdap_mode_snapshot_t mode;
    if (operation.owner != AIRDAP_DAP_OWNER_NONE ||
        airdap_mode_state_get(&mode) != AIRDAP_MODE_STATE_OK || mode.ota != AIRDAP_OTA_IDLE) goto done;

    if (command == AIRDAP_BUTTON_COMMAND_RESTART) {
        bool pressed;
        error = airdap_boot_key_get_pressed(&pressed);
        if (error == ESP_OK && pressed) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) esp_restart();
    } else if (command == AIRDAP_BUTTON_COMMAND_WIFI_TOGGLE) {
        error = airdap_wifi_manager_toggle();
    } else if (command == AIRDAP_BUTTON_COMMAND_TARGET_POWER_TOGGLE) {
        error = airdap_target_power_set_allowed(!airdap_target_power_is_allowed());
    } else {
        const bool reset = command == AIRDAP_BUTTON_COMMAND_TARGET_RESET;
        if (reset && airdap_target_reset_is_asserted()) goto done;
        if (pulse_timer == NULL) {
            const esp_timer_create_args_t args = {.callback = finish_pulse, .name = "airdap_btn_pulse"};
            error = esp_timer_create(&args, &pulse_timer);
            if (error != ESP_OK) goto done;
        }
        const bool previous_power_allowed = airdap_target_power_is_allowed();
        error = reset ? airdap_target_reset_set_asserted(true) : airdap_target_power_set_allowed(false);
        if (error != ESP_OK) goto done;
        pulse_is_reset = reset;
        pulse_operation = operation;
        error = esp_timer_start_once(pulse_timer, reset ? 100000U : 500000U);
        if (error == ESP_OK) return ESP_OK; /* Timer now owns the reservation. */
        const esp_err_t restore_error = reset ? airdap_target_reset_set_asserted(false)
            : airdap_target_power_set_allowed(previous_power_allowed);
        if (restore_error != ESP_OK) {
            ESP_LOGE(TAG, "Button timer failure rollback failed: %s", esp_err_to_name(restore_error));
        }
    }
done:
    if (operation.active) airdap_dap_ownership_operation_end(&operation);
    atomic_store(&busy, false);
    return error;
}
