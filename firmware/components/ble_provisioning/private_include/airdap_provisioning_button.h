#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AIRDAP_PROVISIONING_BUTTON_NONE = 0,
    AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY,
    AIRDAP_PROVISIONING_BUTTON_CLEAR_READY,
    AIRDAP_PROVISIONING_BUTTON_TOGGLE,
    AIRDAP_PROVISIONING_BUTTON_CLEAR,
    AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK,
    AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK,
} airdap_provisioning_button_action_t;

typedef enum {
    AIRDAP_BUTTON_IDLE = 0,
    AIRDAP_BUTTON_PRESS_DEBOUNCE,
    AIRDAP_BUTTON_PRESSED,
    AIRDAP_BUTTON_RELEASE_DEBOUNCE,
    AIRDAP_BUTTON_WAIT_SECOND_PRESS,
    AIRDAP_BUTTON_SECOND_PRESS_DEBOUNCE,
    AIRDAP_BUTTON_SECOND_PRESSED,
    AIRDAP_BUTTON_LONG_3S,
    AIRDAP_BUTTON_LONG_10S,
} airdap_provisioning_button_state_t;

typedef struct {
    airdap_provisioning_button_state_t state;
    uint32_t held_ms;
    uint32_t released_ms;
    uint32_t wait_ms;
    bool second_press;
} airdap_provisioning_button_t;

void airdap_provisioning_button_init(airdap_provisioning_button_t *button);
/* elapsed_ms describes the sampled level's duration. Click presses/releases
 * require 40 ms; the 300 ms double-click window starts at the first release.
 * A second press beginning before that deadline can finish debounce afterward.
 * A long hold cancels pending clicks and retains the 200 ms release guard.
 * State is internal to the polling task; events are delivered to its consumer. */
airdap_provisioning_button_action_t airdap_provisioning_button_step(
    airdap_provisioning_button_t *button,
    bool pressed,
    uint32_t elapsed_ms);
