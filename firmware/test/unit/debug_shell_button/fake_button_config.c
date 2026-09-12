#include <stddef.h>
#include <string.h>
#include "airdap_button_config.h"
#include "airdap_mode_state.h"

static uint8_t commands[5] = {0, 1, 2, 0, 3};
static airdap_dap_route_t route;
esp_err_t fake_button_config_error = ESP_OK;
airdap_mode_dap_result_t fake_dap_route_result = AIRDAP_MODE_DAP_ALLOWED;
esp_err_t airdap_button_config_init(void) { return ESP_OK; }
esp_err_t airdap_button_config_get(uint8_t output[AIRDAP_BUTTON_GESTURE_COUNT])
{
    if (fake_button_config_error != ESP_OK) return fake_button_config_error;
    memcpy(output, commands, sizeof(commands));
    return ESP_OK;
}
esp_err_t airdap_button_config_set(airdap_button_gesture_t gesture, airdap_button_command_t command)
{
    commands[gesture] = (uint8_t) command;
    return ESP_OK;
}
esp_err_t airdap_button_config_defaults(void)
{
    const uint8_t defaults[5] = {0, 1, 2, 0, 3};
    memcpy(commands, defaults, sizeof(commands));
    return ESP_OK;
}
const char *airdap_button_command_name(airdap_button_command_t command)
{
    const char *names[] = {"none", "dap-toggle", "provisioning", "clear-network-restart", "dap-usb", "dap-network", "dap-auto",
        "restart", "target-reset", "wifi-toggle", "target-power-toggle", "target-power-cycle"};
    return command >= 0 && command < AIRDAP_BUTTON_COMMAND_COUNT ? names[command] : NULL;
}
const char *airdap_button_gesture_name(airdap_button_gesture_t gesture)
{
    const char *names[] = {"single", "double", "hold2", "hold6", "hold10"};
    return gesture >= 0 && gesture < AIRDAP_BUTTON_GESTURE_COUNT ? names[gesture] : NULL;
}
airdap_dap_route_t airdap_mode_state_get_dap_route(void) { return route; }
airdap_mode_dap_result_t airdap_mode_state_set_dap_route(airdap_dap_route_t next)
{
    if (fake_dap_route_result != AIRDAP_MODE_DAP_ALLOWED) return fake_dap_route_result;
    route = next == AIRDAP_DAP_ROUTE_TOGGLE ? AIRDAP_DAP_ROUTE_NETWORK : next;
    return AIRDAP_MODE_DAP_ALLOWED;
}
