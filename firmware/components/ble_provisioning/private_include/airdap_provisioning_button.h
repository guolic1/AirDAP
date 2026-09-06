#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AIRDAP_PROVISIONING_BUTTON_NONE = 0,
    AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY,
    AIRDAP_PROVISIONING_BUTTON_CLEAR_READY,
    AIRDAP_PROVISIONING_BUTTON_TOGGLE,
    AIRDAP_PROVISIONING_BUTTON_CLEAR,
} airdap_provisioning_button_action_t;

typedef struct {
    uint32_t held_ms;
    uint32_t released_ms;
    bool active;
} airdap_provisioning_button_t;

void airdap_provisioning_button_init(airdap_provisioning_button_t *button);
airdap_provisioning_button_action_t airdap_provisioning_button_step(
    airdap_provisioning_button_t *button,
    bool pressed,
    uint32_t elapsed_ms);
