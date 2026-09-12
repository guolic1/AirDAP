#include "airdap_button_indicator.h"

enum {
    CLICK_FLASH_MS = 100,
    SLOW_HALF_PERIOD_MS = 500,
    FAST_HALF_PERIOD_MS = 100,
};

void airdap_button_indicator_step(
    airdap_button_indicator_t *indicator,
    const airdap_provisioning_button_t *button,
    airdap_provisioning_button_action_t action,
    uint32_t elapsed_ms)
{
    airdap_button_indicator_pattern_t pattern = indicator->pattern;
    if (action == AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK) {
        pattern = AIRDAP_BUTTON_INDICATOR_SINGLE;
    } else if (action == AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK) {
        pattern = AIRDAP_BUTTON_INDICATOR_DOUBLE;
    } else {
        switch (button->state) {
        case AIRDAP_BUTTON_IDLE:
            if (pattern != AIRDAP_BUTTON_INDICATOR_SINGLE &&
                pattern != AIRDAP_BUTTON_INDICATOR_DOUBLE) {
                pattern = AIRDAP_BUTTON_INDICATOR_OFF;
            }
            break;
        case AIRDAP_BUTTON_WAIT_SECOND_PRESS:
            pattern = AIRDAP_BUTTON_INDICATOR_OFF;
            break;
        case AIRDAP_BUTTON_PRESSED:
        case AIRDAP_BUTTON_SECOND_PRESSED:
            pattern = AIRDAP_BUTTON_INDICATOR_OFF;
            break;
        case AIRDAP_BUTTON_LONG_3S:
            pattern = AIRDAP_BUTTON_INDICATOR_SLOW;
            break;
        case AIRDAP_BUTTON_LONG_10S:
            pattern = AIRDAP_BUTTON_INDICATOR_FAST;
            break;
        case AIRDAP_BUTTON_PRESS_DEBOUNCE:
        case AIRDAP_BUTTON_SECOND_PRESS_DEBOUNCE:
        case AIRDAP_BUTTON_RELEASE_DEBOUNCE:
            break;
        }
    }

    const bool click = action == AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK ||
        action == AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK;
    if (pattern != indicator->pattern || click) {
        indicator->phase_ms = 0U;
    } else {
        /* Bound the phase before addition, including unusually large steps. */
        const uint32_t period = pattern == AIRDAP_BUTTON_INDICATOR_SLOW
            ? 2U * SLOW_HALF_PERIOD_MS : 2U * FAST_HALF_PERIOD_MS;
        if (pattern == AIRDAP_BUTTON_INDICATOR_SINGLE ||
            pattern == AIRDAP_BUTTON_INDICATOR_DOUBLE) {
            const uint32_t duration = pattern == AIRDAP_BUTTON_INDICATOR_SINGLE
                ? CLICK_FLASH_MS : 3U * CLICK_FLASH_MS;
            indicator->phase_ms += elapsed_ms < duration - indicator->phase_ms
                ? elapsed_ms : duration - indicator->phase_ms;
            if (indicator->phase_ms == duration) {
                pattern = AIRDAP_BUTTON_INDICATOR_OFF;
                indicator->phase_ms = 0U;
            }
        } else {
            indicator->phase_ms = (indicator->phase_ms + elapsed_ms % period) % period;
        }
    }
    indicator->pattern = pattern;
    switch (pattern) {
    case AIRDAP_BUTTON_INDICATOR_OFF:
        indicator->status_on = false;
        break;
    case AIRDAP_BUTTON_INDICATOR_SINGLE:
        indicator->status_on = true;
        break;
    case AIRDAP_BUTTON_INDICATOR_DOUBLE:
        indicator->status_on = indicator->phase_ms < CLICK_FLASH_MS ||
            indicator->phase_ms >= 2U * CLICK_FLASH_MS;
        break;
    case AIRDAP_BUTTON_INDICATOR_SLOW:
        indicator->status_on = indicator->phase_ms < SLOW_HALF_PERIOD_MS;
        break;
    case AIRDAP_BUTTON_INDICATOR_FAST:
        indicator->status_on = indicator->phase_ms < FAST_HALF_PERIOD_MS;
        break;
    }
}
