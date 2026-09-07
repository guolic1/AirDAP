#include <stdbool.h>
#include <string.h>

#include "airdap_board.h"
#include "airdap_debug_shell_button.h"
#include "esp_err.h"

static int print_component_error(
    const airdap_debug_shell_invocation_t *invocation,
    const char *operation,
    esp_err_t error)
{
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_ERROR,
        "button: %s failed: %s\n",
        operation,
        esp_err_to_name(error));
    return 1;
}

int airdap_debug_shell_button_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    bool pressed;
    esp_err_t error;

    if (arguments != NULL && strcmp(arguments, "press") == 0) {
        pressed = true;
        error = airdap_boot_key_set_simulated_pressed(pressed);
        if (error != ESP_OK) {
            return print_component_error(invocation, "update", error);
        }
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "button: simulated=pressed\n");
        return 0;
    }
    if (arguments != NULL && strcmp(arguments, "release") == 0) {
        pressed = false;
        error = airdap_boot_key_set_simulated_pressed(pressed);
        if (error != ESP_OK) {
            return print_component_error(invocation, "update", error);
        }
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "button: simulated=released\n");
        return 0;
    }
    if (arguments != NULL && strcmp(arguments, "status") == 0) {
        error = airdap_boot_key_get_simulated_pressed(&pressed);
        if (error != ESP_OK) {
            return print_component_error(invocation, "status", error);
        }
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_DEFAULT,
            "button: simulated=%s\n",
            pressed ? "pressed" : "released");
        return 0;
    }

    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_WARNING,
        "usage: button press|release|status\n");
    return 1;
}
