#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airdap_provisioning_button.h"

enum {
    PROVISIONING_TOGGLE_MS = 3000,
    PROVISIONING_CLEAR_MS = 10000,
    RELEASE_CONFIRM_MS = 200,
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
        if (!button->active) {
            return AIRDAP_PROVISIONING_BUTTON_NONE;
        }
        if (button->released_ms < RELEASE_CONFIRM_MS) {
            const uint32_t remaining =
                RELEASE_CONFIRM_MS - button->released_ms;
            button->released_ms +=
                elapsed_ms < remaining ? elapsed_ms : remaining;
        }
        if (button->released_ms < RELEASE_CONFIRM_MS) {
            return AIRDAP_PROVISIONING_BUTTON_NONE;
        }
        const uint32_t held_ms = button->held_ms;
        airdap_provisioning_button_init(button);
        if (held_ms >= PROVISIONING_CLEAR_MS) {
            return AIRDAP_PROVISIONING_BUTTON_CLEAR;
        }
        return held_ms >= PROVISIONING_TOGGLE_MS
            ? AIRDAP_PROVISIONING_BUTTON_TOGGLE
            : AIRDAP_PROVISIONING_BUTTON_NONE;
    }

    button->active = true;
    button->released_ms = 0U;
    const uint32_t previous_held_ms = button->held_ms;
    if (button->held_ms < PROVISIONING_CLEAR_MS) {
        const uint32_t remaining = PROVISIONING_CLEAR_MS - button->held_ms;
        button->held_ms += elapsed_ms < remaining ? elapsed_ms : remaining;
    }
    if (previous_held_ms < PROVISIONING_CLEAR_MS &&
        button->held_ms >= PROVISIONING_CLEAR_MS) {
        return AIRDAP_PROVISIONING_BUTTON_CLEAR_READY;
    }
    if (previous_held_ms < PROVISIONING_TOGGLE_MS &&
        button->held_ms >= PROVISIONING_TOGGLE_MS) {
        return AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY;
    }
    return AIRDAP_PROVISIONING_BUTTON_NONE;
}
