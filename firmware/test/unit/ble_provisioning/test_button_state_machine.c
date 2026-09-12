#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_provisioning_button.h"

typedef airdap_provisioning_button_t Button;
typedef airdap_provisioning_button_action_t Action;

static void step(Button *button, bool pressed, uint32_t ms, Action expected)
{
    assert(airdap_provisioning_button_step(button, pressed, ms) == expected);
}

#define NONE AIRDAP_PROVISIONING_BUTTON_NONE
#define SINGLE AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK
#define DOUBLE AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK
#define READY3 AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY
#define READY10 AIRDAP_PROVISIONING_BUTTON_CLEAR_READY
#define LONG3 AIRDAP_PROVISIONING_BUTTON_TOGGLE
#define LONG10 AIRDAP_PROVISIONING_BUTTON_CLEAR

static Button fresh(void)
{
    Button button;
    airdap_provisioning_button_init(&button);
    assert(button.state == AIRDAP_BUTTON_IDLE);
    return button;
}

static void first_click(Button *button)
{
    step(button, true, 40, NONE);
    assert(button->state == AIRDAP_BUTTON_PRESSED);
    step(button, false, 40, NONE);
    assert(button->state == AIRDAP_BUTTON_WAIT_SECOND_PRESS);
}

static void test_debounce_and_single_deadline(void)
{
    Button button = fresh();
    step(&button, true, 20, NONE);
    assert(button.state == AIRDAP_BUTTON_PRESS_DEBOUNCE);
    step(&button, false, 20, NONE);
    assert(button.state == AIRDAP_BUTTON_IDLE);
    step(&button, false, 1000, NONE);
    step(&button, true, 20, NONE);
    step(&button, true, 20, NONE);
    assert(button.state == AIRDAP_BUTTON_PRESSED);
    step(&button, false, 20, NONE);
    assert(button.state == AIRDAP_BUTTON_RELEASE_DEBOUNCE);
    step(&button, false, 20, NONE);
    step(&button, false, 259, NONE);
    step(&button, false, 1, SINGLE);
    assert(button.state == AIRDAP_BUTTON_IDLE);
    step(&button, false, 1000, NONE);
}

static void test_double_suppresses_single(void)
{
    Button button = fresh();
    first_click(&button);
    step(&button, false, 100, NONE);
    step(&button, true, 20, NONE);
    assert(button.state == AIRDAP_BUTTON_SECOND_PRESS_DEBOUNCE);
    step(&button, true, 20, NONE);
    assert(button.state == AIRDAP_BUTTON_SECOND_PRESSED);
    step(&button, false, 39, NONE);
    step(&button, false, 1, DOUBLE);
    assert(button.state == AIRDAP_BUTTON_IDLE);
    step(&button, false, 1000, NONE);
    first_click(&button);
    step(&button, false, 260, SINGLE);
}

static void test_second_press_bounce_preserves_single(void)
{
    Button button = fresh();
    first_click(&button);
    step(&button, false, 200, NONE);
    step(&button, true, 20, NONE);
    step(&button, false, 39, NONE);
    step(&button, false, 1, SINGLE);

    button = fresh();
    first_click(&button);
    step(&button, false, 259, NONE);
    step(&button, true, 20, NONE);
    step(&button, true, 20, NONE);
    step(&button, false, 40, DOUBLE);
}

static void test_release_bounce_keeps_one_press(void)
{
    Button button = fresh();
    step(&button, true, 40, NONE);
    step(&button, false, 20, NONE);
    step(&button, true, 20, NONE);
    step(&button, false, 300, SINGLE);
    step(&button, false, 300, NONE);
}

static void test_long_hold_suppresses_clicks_and_waits_for_release(void)
{
    Button button = fresh();
    first_click(&button);
    step(&button, true, 2999, NONE);
    step(&button, true, 1, READY3);
    assert(button.state == AIRDAP_BUTTON_LONG_3S);
    step(&button, true, 6999, NONE);
    step(&button, true, 1, READY10);
    assert(button.state == AIRDAP_BUTTON_LONG_10S);
    step(&button, true, UINT32_MAX, NONE);
    step(&button, false, 199, NONE);
    step(&button, true, 20, NONE);
    step(&button, false, 200, LONG10);
    step(&button, false, 1000, NONE);

    button = fresh();
    step(&button, true, 3000, READY3);
    step(&button, false, 200, LONG3);
    step(&button, false, 1000, NONE);
}

static void test_invalid_input_and_saturating_time(void)
{
    Button button = fresh();
    Button before = button;
    step(NULL, true, 1, NONE);
    airdap_provisioning_button_init(NULL);
    step(&button, true, 0, NONE);
    assert(memcmp(&before, &button, sizeof(button)) == 0);
    step(&button, true, UINT32_MAX, READY10);
    step(&button, true, UINT32_MAX, NONE);
    step(&button, false, UINT32_MAX, LONG10);
    first_click(&button);
    step(&button, false, UINT32_MAX, SINGLE);
}

static void test_late_second_press_and_long_boundaries(void)
{
    Button button = fresh();
    first_click(&button);
    step(&button, false, 260, SINGLE);
    first_click(&button);
    step(&button, false, 260, SINGLE);

    button = fresh();
    step(&button, true, 2999, NONE);
    step(&button, false, 300, SINGLE);
    step(&button, true, 3000, READY3);
    step(&button, true, 6999, NONE);
    step(&button, false, 200, LONG3);
    step(&button, false, 300, NONE);
}

int main(void)
{
    test_debounce_and_single_deadline();
    test_double_suppresses_single();
    test_second_press_bounce_preserves_single();
    test_release_bounce_keeps_one_press();
    test_long_hold_suppresses_clicks_and_waits_for_release();
    test_invalid_input_and_saturating_time();
    test_late_second_press_and_long_boundaries();
    puts("Button state machine tests passed");
    return 0;
}
