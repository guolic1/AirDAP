#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "airdap_button_simulation.h"
#include "airdap_button_config.h"
#include "airdap_mode_state.h"
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
    airdap_button_gesture_t gesture;
    esp_err_t error;

    if (arguments != NULL && strcmp(arguments, "commands") == 0) {
        for (int command = 0; command < AIRDAP_BUTTON_COMMAND_COUNT; ++command) {
            airdap_debug_shell_printf(invocation, AIRDAP_DEBUG_SHELL_STYLE_DEFAULT,
                "%s\n", airdap_button_command_name((airdap_button_command_t) command));
        }
        return 0;
    }
    if (arguments != NULL && strcmp(arguments, "bindings") == 0) {
        uint8_t commands[AIRDAP_BUTTON_GESTURE_COUNT];
        error = airdap_button_config_get(commands);
        if (error != ESP_OK) return print_component_error(invocation, "bindings", error);
        for (int gesture = 0; gesture < AIRDAP_BUTTON_GESTURE_COUNT; ++gesture) {
            airdap_debug_shell_printf(invocation, AIRDAP_DEBUG_SHELL_STYLE_DEFAULT,
                "%s=%s\n", airdap_button_gesture_name((airdap_button_gesture_t) gesture),
                airdap_button_command_name((airdap_button_command_t) commands[gesture]));
        }
        const airdap_dap_route_t route = airdap_mode_state_get_dap_route();
        airdap_debug_shell_printf(invocation, AIRDAP_DEBUG_SHELL_STYLE_DEFAULT,
            "dap-route=%s (volatile)\n", route == AIRDAP_DAP_ROUTE_USB ? "usb" :
                route == AIRDAP_DAP_ROUTE_NETWORK ? "network" : "auto");
        return 0;
    }
    if (arguments != NULL && strcmp(arguments, "defaults") == 0) {
        error = airdap_button_config_defaults();
        if (error != ESP_OK) return print_component_error(invocation, "save", error);
        airdap_debug_shell_printf(invocation, AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "button: default bindings saved\n");
        return 0;
    }
    if (arguments != NULL && strncmp(arguments, "bind ", 5) == 0) {
        char gesture_name[16], command_name[32], extra;
        if (sscanf(arguments + 5, "%15s %31s %c", gesture_name, command_name, &extra) == 2) {
            for (int gesture = 0; gesture < AIRDAP_BUTTON_GESTURE_COUNT; ++gesture) {
                if (strcmp(gesture_name, airdap_button_gesture_name((airdap_button_gesture_t) gesture)) != 0) continue;
                for (int command = 0; command < AIRDAP_BUTTON_COMMAND_COUNT; ++command) {
                    if (strcmp(command_name, airdap_button_command_name((airdap_button_command_t) command)) != 0) continue;
                    error = airdap_button_config_set((airdap_button_gesture_t) gesture, (airdap_button_command_t) command);
                    if (error != ESP_OK) return print_component_error(invocation, "save", error);
                    airdap_debug_shell_printf(invocation, AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
                        "button: %s=%s saved\n", gesture_name, command_name);
                    return 0;
                }
            }
        }
    }

    if (arguments != NULL && strncmp(arguments, "simulate ", 9) == 0) {
        for (int value = 0; value < AIRDAP_BUTTON_GESTURE_COUNT; ++value) {
            const char *name = airdap_button_gesture_name((airdap_button_gesture_t) value);
            if (strcmp(arguments + 9, name) != 0) continue;
            error = airdap_button_simulate((airdap_button_gesture_t) value);
            if (error != ESP_OK) return print_component_error(invocation, "simulate", error);
            airdap_debug_shell_printf(invocation, AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
                "button: simulation=%s accepted\n", name);
            return 0;
        }
    }
    if (arguments != NULL && strcmp(arguments, "status") == 0) {
        error = airdap_button_simulation_get(&gesture);
        if (error != ESP_OK) {
            return print_component_error(invocation, "status", error);
        }
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_DEFAULT,
            "button: simulation=%s\n",
            gesture == AIRDAP_BUTTON_GESTURE_COUNT ? "idle" : airdap_button_gesture_name(gesture));
        return 0;
    }

    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_WARNING,
        "usage: button simulate <single|double|hold2|hold6|hold10>|status|commands|bindings|defaults|bind <gesture> <command>\n");
    return 1;
}
