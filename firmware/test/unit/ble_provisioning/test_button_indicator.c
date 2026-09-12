#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_button_indicator.h"

static airdap_button_indicator_t indicator;
static airdap_provisioning_button_t button;

static void step(airdap_provisioning_button_state_t state,
    airdap_provisioning_button_action_t action, uint32_t ms,
    bool status)
{
    button.state = state;
    airdap_button_indicator_step(&indicator, &button, action, ms);
    assert(indicator.status_on == status);
}

int main(void)
{
    const airdap_provisioning_button_action_t none = AIRDAP_PROVISIONING_BUTTON_NONE;
    step(AIRDAP_BUTTON_IDLE, none, 20, false);
    step(AIRDAP_BUTTON_PRESS_DEBOUNCE, none, 20, false);
    step(AIRDAP_BUTTON_PRESSED, none, 20, false);
    step(AIRDAP_BUTTON_RELEASE_DEBOUNCE, none, 20, false);
    step(AIRDAP_BUTTON_WAIT_SECOND_PRESS, none, 20, false);
    step(AIRDAP_BUTTON_SECOND_PRESS_DEBOUNCE, none, 20, false);
    step(AIRDAP_BUTTON_SECOND_PRESSED, none, 20, false);
    step(AIRDAP_BUTTON_IDLE, AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK, 20, true);
    step(AIRDAP_BUTTON_IDLE, none, 99, true);
    step(AIRDAP_BUTTON_IDLE, none, 1, false);
    step(AIRDAP_BUTTON_IDLE, none, 100, true);
    step(AIRDAP_BUTTON_IDLE, none, 100, false);
    step(AIRDAP_BUTTON_IDLE, none, 1000, false);
    step(AIRDAP_BUTTON_IDLE, AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK, 20, true);
    step(AIRDAP_BUTTON_IDLE, none, 99, true);
    step(AIRDAP_BUTTON_IDLE, none, 1, false);
    step(AIRDAP_BUTTON_IDLE, none, 200, false);

    button.held_ms = 3000;
    step(AIRDAP_BUTTON_LONG_3S, none, 20, true);
    step(AIRDAP_BUTTON_LONG_3S, none, 499, true);
    step(AIRDAP_BUTTON_RELEASE_DEBOUNCE, none, 1, false);
    step(AIRDAP_BUTTON_LONG_3S, none, 500, true);
    button.held_ms = 10000;
    step(AIRDAP_BUTTON_LONG_10S, none, 20, true);
    step(AIRDAP_BUTTON_LONG_10S, none, 99, true);
    step(AIRDAP_BUTTON_RELEASE_DEBOUNCE, none, 1, false);
    step(AIRDAP_BUTTON_LONG_10S, none, 100, true);
    step(AIRDAP_BUTTON_IDLE, AIRDAP_PROVISIONING_BUTTON_CLEAR, 20, false);

    button.held_ms = 0;
    step(AIRDAP_BUTTON_IDLE, AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK, 20, true);
    step(AIRDAP_BUTTON_PRESS_DEBOUNCE, none, 100, false);
    step(AIRDAP_BUTTON_PRESSED, none, 20, false);
    step(AIRDAP_BUTTON_WAIT_SECOND_PRESS, none, 20, false);
    step(AIRDAP_BUTTON_IDLE, none, 1000, false);
    step(AIRDAP_BUTTON_IDLE, AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK, 20, true);
    step(AIRDAP_BUTTON_IDLE, none, UINT32_MAX, false);
    puts("button indicator tests passed");
    return 0;
}
