#pragma once

#include "airdap_provisioning_button.h"

typedef enum {
    AIRDAP_BUTTON_INDICATOR_OFF = 0,
    AIRDAP_BUTTON_INDICATOR_SINGLE,
    AIRDAP_BUTTON_INDICATOR_DOUBLE,
    AIRDAP_BUTTON_INDICATOR_SLOW,
    AIRDAP_BUTTON_INDICATOR_FAST,
} airdap_button_indicator_pattern_t;

typedef struct {
    airdap_button_indicator_pattern_t pattern;
    uint32_t phase_ms;
    bool status_on;
} airdap_button_indicator_t;

/* Zero-initialize once; call from the button polling task after recognition.
 * Debounce preserves the previous pattern. A confirmed press interrupts any
 * completion flash. No timers, GPIO writes, or provisioning actions run here. */
void airdap_button_indicator_step(
    airdap_button_indicator_t *indicator,
    const airdap_provisioning_button_t *button,
    airdap_provisioning_button_action_t action,
    uint32_t elapsed_ms);
