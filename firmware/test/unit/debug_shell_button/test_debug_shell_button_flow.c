#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "airdap_board.h"
#include "airdap_board_pins.h"
#include "airdap_button_simulator.h"
#include "airdap_debug_shell_button.h"
#include "airdap_provisioning_button.h"
#include "driver/gpio.h"

static int boot_key_level = 1;
static airdap_provisioning_button_t button;
static airdap_button_simulator_t simulator;
static unsigned actions[9];
static unsigned cancelled;
esp_err_t gpio_config(const gpio_config_t *config) { (void) config; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { (void) pin; (void) level; return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin == AIRDAP_PIN_BOOT_KEY); return boot_key_level; }
static void discard(airdap_debug_shell_style_t style, const char *format, va_list args, void *context)
{ (void) style; (void) format; (void) args; (void) context; }
static int run(const char *arguments)
{
    const airdap_debug_shell_invocation_t invocation = {.vprintf = discard};
    return airdap_debug_shell_button_command(arguments, &invocation, NULL);
}
static void poll(bool valid)
{
    bool physical, simulated;
    assert(airdap_boot_key_get_pressed(&physical) == ESP_OK);
    if (airdap_button_simulation_step(&simulator, physical, valid,
            button.state == AIRDAP_BUTTON_IDLE, 20, &simulated)) {
        airdap_provisioning_button_init(&button);
        ++cancelled;
    }
    if (valid) ++actions[airdap_provisioning_button_step(&button, physical || simulated, 20)];
}
static void polls(unsigned count) { while (count-- != 0) poll(true); }
static void reset(void)
{
    boot_key_level = 1;
    simulator = (airdap_button_simulator_t) {0};
    airdap_provisioning_button_init(&button);
    airdap_button_simulation_init();
    memset(actions, 0, sizeof(actions));
    cancelled = 0;
}
static void assert_only_terminal(airdap_provisioning_button_action_t expected, unsigned count)
{
    const airdap_provisioning_button_action_t terminal[] = {AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK,
        AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK, AIRDAP_PROVISIONING_BUTTON_TOGGLE,
        AIRDAP_PROVISIONING_BUTTON_HOLD_6, AIRDAP_PROVISIONING_BUTTON_CLEAR};
    for (unsigned i = 0; i < sizeof(terminal) / sizeof(terminal[0]); ++i) {
        assert(actions[terminal[i]] == (terminal[i] == expected ? count : 0));
    }
}
int main(void)
{
    airdap_button_gesture_t active;
    assert(airdap_button_simulate(AIRDAP_BUTTON_GESTURE_SINGLE) == ESP_ERR_INVALID_STATE);
    assert(airdap_button_simulation_get(&active) == ESP_ERR_INVALID_STATE);
    assert(airdap_button_simulation_get(NULL) == ESP_ERR_INVALID_ARG);
    const char *names[] = {"single", "double", "hold2", "hold6", "hold10"};
    const airdap_provisioning_button_action_t expected[] = {AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK,
        AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK, AIRDAP_PROVISIONING_BUTTON_TOGGLE,
        AIRDAP_PROVISIONING_BUTTON_HOLD_6, AIRDAP_PROVISIONING_BUTTON_CLEAR};
    for (int g = 0; g < AIRDAP_BUTTON_GESTURE_COUNT; ++g) {
        reset();
        char command[40];
        snprintf(command, sizeof(command), "simulate %s", names[g]);
        assert(run(command) == 0);
        assert(run("simulate single") == 1); /* Includes queued requests. */
        assert(airdap_button_simulation_get(&active) == ESP_OK && active == (airdap_button_gesture_t) g);
        polls(g < 2 ? 40 : g == 2 ? 130 : g == 3 ? 330 : 530);
        assert_only_terminal(expected[g], 1);
        assert(actions[AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY] == (g >= 2 ? 1U : 0U));
        assert(actions[AIRDAP_PROVISIONING_BUTTON_HOLD_6_READY] == (g >= 3 ? 1U : 0U));
        assert(actions[AIRDAP_PROVISIONING_BUTTON_CLEAR_READY] == (g >= 4 ? 1U : 0U));
        assert(airdap_button_simulation_get(&active) == ESP_OK && active == AIRDAP_BUTTON_GESTURE_COUNT);
        polls(30);
        assert_only_terminal(expected[g], 1); /* No latched press or repeat. */
        assert(cancelled == 0);
    }
    reset();
    boot_key_level = 0;
    poll(true);
    assert(run("simulate hold10") == 1);
    boot_key_level = 1;
    polls(30);
    reset();
    assert(run("simulate hold6") == 0);
    polls(110); /* Cross hold2, then physical input must discard that hold. */
    boot_key_level = 0;
    polls(4);
    assert(cancelled == 1);
    boot_key_level = 1;
    polls(30);
    assert_only_terminal(AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK, 1);
    reset();
    assert(run("simulate hold10") == 0);
    polls(310);
    poll(false);
    polls(40);
    assert(cancelled == 1);
    assert_only_terminal(AIRDAP_PROVISIONING_BUTTON_NONE, 0);
    assert(run("simulate double") == 0);
    polls(40);
    assert_only_terminal(AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK, 1);
    reset();
    assert(run("simulate hold10") == 0);
    boot_key_level = 0; /* Press races with the queued shell request. */
    poll(true);
    assert(cancelled == 1);
    assert(airdap_button_simulate(AIRDAP_BUTTON_GESTURE_COUNT) == ESP_ERR_INVALID_ARG);
    reset();
    (void) airdap_provisioning_button_step(&button, true, 2100);
    assert(run("simulate single") == 0); /* Request before physical-busy publication. */
    polls(30);
    assert_only_terminal(AIRDAP_PROVISIONING_BUTTON_TOGGLE, 1);
    assert(cancelled == 0);
    puts("Shell simulation through physical input and recognizer passed");
    return 0;
}
