#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airdap_provisioning_button.h"

enum {
    PROVISIONING_TOGGLE_MS = 3000,
    PROVISIONING_CLEAR_MS = 10000,
};

void airdap_provisioning_button_init(airdap_provisioning_button_t *button)
{
    if (button != NULL) {
        memset(button, 0, sizeof(*button));
    }
}

airdap_provisioning_button_action_t airdap_provisioning_button_step(
    airdap_provisioning_button_t *button,
    bool pressed,
    uint32_t elapsed_ms)
{
    if (button == NULL || elapsed_ms == 0U) {
        return AIRDAP_PROVISIONING_BUTTON_NONE;
    }
    if (!pressed) {
        if (!button->pressed) {
            return AIRDAP_PROVISIONING_BUTTON_NONE;
        }
        airdap_provisioning_button_init(button);
        return AIRDAP_PROVISIONING_BUTTON_RELEASED;
    }

    button->pressed = true;
    if (button->held_ms < PROVISIONING_CLEAR_MS) {
        const uint32_t remaining = PROVISIONING_CLEAR_MS - button->held_ms;
        button->held_ms += elapsed_ms < remaining ? elapsed_ms : remaining;
    }
    if (!button->toggle_emitted &&
        button->held_ms >= PROVISIONING_TOGGLE_MS) {
        button->toggle_emitted = true;
        return AIRDAP_PROVISIONING_BUTTON_TOGGLE;
    }
    if (!button->clear_emitted &&
        button->held_ms >= PROVISIONING_CLEAR_MS) {
        button->clear_emitted = true;
        return AIRDAP_PROVISIONING_BUTTON_CLEAR;
    }
    return AIRDAP_PROVISIONING_BUTTON_NONE;
}
