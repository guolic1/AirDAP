#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airdap_provisioning_button.h"

enum {
    PRESS_CONFIRM_MS = 40,
    CLICK_RELEASE_MS = 40,
    DOUBLE_CLICK_MS = 300,
    PROVISIONING_TOGGLE_MS = 2000,
    HOLD_6_MS = 6000,
    PROVISIONING_CLEAR_MS = 10000,
    RELEASE_CONFIRM_MS = 200,
};

void airdap_provisioning_button_init(airdap_provisioning_button_t *button)
{
    if (button != NULL) {
        memset(button, 0, sizeof(*button));
    }
}

static uint32_t add_capped(uint32_t value, uint32_t elapsed, uint32_t limit)
{
    return value + (elapsed < limit - value ? elapsed : limit - value);
}

static airdap_provisioning_button_action_t finish(
    airdap_provisioning_button_t *button,
    airdap_provisioning_button_action_t action)
{
    airdap_provisioning_button_init(button);
    return action;
}

static airdap_provisioning_button_action_t hold(
    airdap_provisioning_button_t *button, uint32_t elapsed_ms)
{
    const uint32_t previous = button->held_ms;
    button->held_ms = add_capped(previous, elapsed_ms, PROVISIONING_CLEAR_MS);
    button->released_ms = 0U;
    if (button->held_ms >= PROVISIONING_CLEAR_MS) {
        button->state = AIRDAP_BUTTON_LONG_10S;
        return previous < PROVISIONING_CLEAR_MS
            ? AIRDAP_PROVISIONING_BUTTON_CLEAR_READY
            : AIRDAP_PROVISIONING_BUTTON_NONE;
    }
    if (button->held_ms >= HOLD_6_MS) {
        button->state = AIRDAP_BUTTON_LONG_6S;
        return previous < HOLD_6_MS
            ? AIRDAP_PROVISIONING_BUTTON_HOLD_6_READY
            : AIRDAP_PROVISIONING_BUTTON_NONE;
    }
    if (button->held_ms >= PROVISIONING_TOGGLE_MS) {
        button->state = AIRDAP_BUTTON_LONG_2S;
        return previous < PROVISIONING_TOGGLE_MS
            ? AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY
            : AIRDAP_PROVISIONING_BUTTON_NONE;
    }
    if (button->held_ms >= PRESS_CONFIRM_MS) {
        button->state = button->second_press
            ? AIRDAP_BUTTON_SECOND_PRESSED : AIRDAP_BUTTON_PRESSED;
    }
    return AIRDAP_PROVISIONING_BUTTON_NONE;
}

static airdap_provisioning_button_action_t wait_for_second(
    airdap_provisioning_button_t *button, uint32_t elapsed_ms)
{
    button->wait_ms = add_capped(button->wait_ms, elapsed_ms, DOUBLE_CLICK_MS);
    if (button->wait_ms == DOUBLE_CLICK_MS) {
        return finish(button, AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK);
    }
    button->state = AIRDAP_BUTTON_WAIT_SECOND_PRESS;
    return AIRDAP_PROVISIONING_BUTTON_NONE;
}

static airdap_provisioning_button_action_t release(
    airdap_provisioning_button_t *button, uint32_t elapsed_ms)
{
    const bool long_hold = button->held_ms >= PROVISIONING_TOGGLE_MS;
    button->released_ms = add_capped(
        button->released_ms, elapsed_ms,
        long_hold ? RELEASE_CONFIRM_MS : DOUBLE_CLICK_MS);
    button->state = AIRDAP_BUTTON_RELEASE_DEBOUNCE;
    if (button->released_ms < (long_hold ? RELEASE_CONFIRM_MS : CLICK_RELEASE_MS)) {
        return AIRDAP_PROVISIONING_BUTTON_NONE;
    }
    if (long_hold) {
        return finish(button, button->held_ms >= PROVISIONING_CLEAR_MS
            ? AIRDAP_PROVISIONING_BUTTON_CLEAR
            : button->held_ms >= HOLD_6_MS ? AIRDAP_PROVISIONING_BUTTON_HOLD_6
            : AIRDAP_PROVISIONING_BUTTON_TOGGLE);
    }
    if (button->second_press) {
        return finish(button, AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK);
    }
    /* Include release debounce in the click window, not an extra delay. */
    button->wait_ms = 0U;
    return wait_for_second(button, button->released_ms);
}

airdap_provisioning_button_action_t airdap_provisioning_button_step(
    airdap_provisioning_button_t *button, bool pressed, uint32_t elapsed_ms)
{
    if (button == NULL || elapsed_ms == 0U) {
        return AIRDAP_PROVISIONING_BUTTON_NONE;
    }
    switch (button->state) {
    case AIRDAP_BUTTON_IDLE:
        if (!pressed) return AIRDAP_PROVISIONING_BUTTON_NONE;
        button->state = AIRDAP_BUTTON_PRESS_DEBOUNCE;
        return hold(button, elapsed_ms);
    case AIRDAP_BUTTON_WAIT_SECOND_PRESS:
        if (!pressed) return wait_for_second(button, elapsed_ms);
        button->second_press = true;
        button->held_ms = 0U;
        button->state = AIRDAP_BUTTON_SECOND_PRESS_DEBOUNCE;
        /* Retain the first click's deadline in case this candidate bounces. */
        button->wait_ms = add_capped(button->wait_ms, elapsed_ms, DOUBLE_CLICK_MS);
        return hold(button, elapsed_ms);
    case AIRDAP_BUTTON_PRESS_DEBOUNCE:
        return pressed ? hold(button, elapsed_ms)
            : finish(button, AIRDAP_PROVISIONING_BUTTON_NONE);
    case AIRDAP_BUTTON_SECOND_PRESS_DEBOUNCE:
        if (!pressed) {
            return wait_for_second(button, elapsed_ms);
        }
        button->wait_ms = add_capped(button->wait_ms, elapsed_ms, DOUBLE_CLICK_MS);
        return hold(button, elapsed_ms);
    case AIRDAP_BUTTON_PRESSED:
    case AIRDAP_BUTTON_SECOND_PRESSED:
    case AIRDAP_BUTTON_LONG_2S:
    case AIRDAP_BUTTON_LONG_6S:
    case AIRDAP_BUTTON_LONG_10S:
    case AIRDAP_BUTTON_RELEASE_DEBOUNCE:
        return pressed ? hold(button, elapsed_ms) : release(button, elapsed_ms);
    }
    return AIRDAP_PROVISIONING_BUTTON_NONE;
}
