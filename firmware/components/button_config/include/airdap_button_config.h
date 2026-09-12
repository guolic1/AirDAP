#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIRDAP_BUTTON_GESTURE_SINGLE = 0,
    AIRDAP_BUTTON_GESTURE_DOUBLE,
    AIRDAP_BUTTON_GESTURE_HOLD2,
    AIRDAP_BUTTON_GESTURE_HOLD6,
    AIRDAP_BUTTON_GESTURE_HOLD10,
    AIRDAP_BUTTON_GESTURE_COUNT,
} airdap_button_gesture_t;

/* Persistent command IDs: append new values; never reorder existing ones. */
typedef enum {
    AIRDAP_BUTTON_COMMAND_NONE = 0,
    AIRDAP_BUTTON_COMMAND_DAP_TOGGLE,
    AIRDAP_BUTTON_COMMAND_PROVISIONING,
    AIRDAP_BUTTON_COMMAND_CLEAR_NETWORK_RESTART,
    AIRDAP_BUTTON_COMMAND_DAP_USB,
    AIRDAP_BUTTON_COMMAND_DAP_NETWORK,
    AIRDAP_BUTTON_COMMAND_DAP_AUTO,
    AIRDAP_BUTTON_COMMAND_RESTART,
    AIRDAP_BUTTON_COMMAND_TARGET_RESET,
    AIRDAP_BUTTON_COMMAND_WIFI_TOGGLE,
    AIRDAP_BUTTON_COMMAND_TARGET_POWER_TOGGLE,
    AIRDAP_BUTTON_COMMAND_TARGET_POWER_CYCLE,
    AIRDAP_BUTTON_COMMAND_COUNT,
} airdap_button_command_t;

/* Call after config_store has initialized NVS. No global NVS init/erase here. */
esp_err_t airdap_button_config_init(void);
esp_err_t airdap_button_config_get(uint8_t commands[AIRDAP_BUTTON_GESTURE_COUNT]);
/* Persists before publishing the new binding; leaves RAM unchanged on failure. */
esp_err_t airdap_button_config_set(airdap_button_gesture_t gesture, airdap_button_command_t command);
esp_err_t airdap_button_config_defaults(void);
const char *airdap_button_command_name(airdap_button_command_t command);
const char *airdap_button_gesture_name(airdap_button_gesture_t gesture);

#ifdef __cplusplus
}
#endif
