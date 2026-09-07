#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_board.h"
#include "airdap_board_pins.h"
#include "airdap_debug_shell_button.h"
#include "airdap_debug_shell_commands.h"
#include "airdap_provisioning_button.h"
#include "driver/gpio.h"

static int boot_key_level = 1;

esp_err_t gpio_config(const gpio_config_t *config)
{
    (void) config;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    (void) gpio_num;
    (void) level;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    assert(gpio_num == AIRDAP_PIN_BOOT_KEY);
    return boot_key_level;
}

static void discard_vprintf(
    airdap_debug_shell_style_t style,
    const char *format,
    va_list arguments,
    void *context)
{
    (void) style;
    (void) format;
    (void) arguments;
    (void) context;
}

static int run_command(const char *arguments)
{
    const airdap_debug_shell_invocation_t invocation = {
        .vprintf = discard_vprintf,
    };
    return airdap_debug_shell_button_command(arguments, &invocation, NULL);
}

static airdap_provisioning_button_action_t poll_button(
    airdap_provisioning_button_t *button)
{
    bool pressed = false;
    assert(airdap_boot_key_get_pressed(&pressed) == ESP_OK);
    return airdap_provisioning_button_step(button, pressed, 100U);
}

static void assert_hold_actions(
    airdap_provisioning_button_t *button,
    unsigned int poll_count,
    bool expect_clear)
{
    for (unsigned int count = 1U; count <= poll_count; ++count) {
        const airdap_provisioning_button_action_t action = poll_button(button);
        if (count == 30U) {
            assert(action == AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY);
        } else if (expect_clear && count == 100U) {
            assert(action == AIRDAP_PROVISIONING_BUTTON_CLEAR_READY);
        } else {
            assert(action == AIRDAP_PROVISIONING_BUTTON_NONE);
        }
    }
}

static void test_usb_commands_drive_toggle_and_clear_actions(void)
{
    airdap_provisioning_button_t button;
    airdap_provisioning_button_init(&button);
    boot_key_level = 1;

    assert(run_command("press") == 0);
    assert_hold_actions(&button, 30U, false);
    assert(run_command("status") == 0);
    assert(run_command("release") == 0);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_NONE);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_TOGGLE);

    airdap_provisioning_button_init(&button);
    assert(run_command("press") == 0);
    assert_hold_actions(&button, 100U, true);
    assert(run_command("release") == 0);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_NONE);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_CLEAR);
}

static void test_physical_and_simulated_sources_share_release_semantics(void)
{
    airdap_provisioning_button_t button;
    airdap_provisioning_button_init(&button);
    boot_key_level = 1;

    assert(run_command("press") == 0);
    assert_hold_actions(&button, 30U, false);
    boot_key_level = 0;
    assert(run_command("release") == 0);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_NONE);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_NONE);

    boot_key_level = 1;
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_NONE);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_TOGGLE);

    airdap_provisioning_button_init(&button);
    boot_key_level = 0;
    assert_hold_actions(&button, 30U, false);
    boot_key_level = 1;
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_NONE);
    assert(poll_button(&button) == AIRDAP_PROVISIONING_BUTTON_TOGGLE);
}

int main(void)
{
    test_usb_commands_drive_toggle_and_clear_actions();
    test_physical_and_simulated_sources_share_release_semantics();
    puts("Debug shell button flow tests passed");
    return 0;
}
