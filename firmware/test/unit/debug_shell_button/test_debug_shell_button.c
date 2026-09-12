#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "airdap_board.h"
#include "airdap_debug_shell_button.h"
#include "airdap_debug_shell_commands.h"

typedef struct {
    char text[256];
    airdap_debug_shell_style_t style;
} captured_output_t;

static bool simulated_pressed;
static unsigned int set_calls;
static unsigned int get_calls;
static esp_err_t set_result = ESP_OK;
static esp_err_t get_result = ESP_OK;

static void reset_fake_button(void)
{
    simulated_pressed = false;
    set_calls = 0U;
    get_calls = 0U;
    set_result = ESP_OK;
    get_result = ESP_OK;
}

esp_err_t airdap_boot_key_set_simulated_pressed(bool pressed)
{
    ++set_calls;
    if (set_result == ESP_OK) {
        simulated_pressed = pressed;
    }
    return set_result;
}

esp_err_t airdap_boot_key_get_simulated_pressed(bool *pressed)
{
    assert(pressed != NULL);
    ++get_calls;
    if (get_result == ESP_OK) {
        *pressed = simulated_pressed;
    }
    return get_result;
}

static void capture_vprintf(
    airdap_debug_shell_style_t style,
    const char *format,
    va_list arguments,
    void *context)
{
    captured_output_t *output = context;
    output->style = style;
    const size_t used = strlen(output->text);
    (void) vsnprintf(
        output->text + used,
        sizeof(output->text) - used,
        format,
        arguments);
}

static int run_command(const char *arguments, captured_output_t *output)
{
    const airdap_debug_shell_invocation_t invocation = {
        .vprintf = capture_vprintf,
        .output_context = output,
    };
    return airdap_debug_shell_button_command(arguments, &invocation, NULL);
}

static void test_press_release_and_status_are_idempotent(void)
{
    reset_fake_button();
    captured_output_t output = {0};
    assert(run_command("press", &output) == 0);
    assert(simulated_pressed && set_calls == 1U && get_calls == 0U);
    assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_SUCCESS);
    assert(strcmp(output.text, "button: simulated=pressed\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command("press", &output) == 0);
    assert(simulated_pressed && set_calls == 2U);

    output = (captured_output_t) {0};
    assert(run_command("status", &output) == 0);
    assert(simulated_pressed && set_calls == 2U && get_calls == 1U);
    assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_DEFAULT);
    assert(strcmp(output.text, "button: simulated=pressed\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command("release", &output) == 0);
    assert(!simulated_pressed && set_calls == 3U);
    assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_SUCCESS);
    assert(strcmp(output.text, "button: simulated=released\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command("status", &output) == 0);
    assert(!simulated_pressed && get_calls == 2U);
    assert(strcmp(output.text, "button: simulated=released\n") == 0);
}

static void test_invalid_arguments_do_not_change_state(void)
{
    static const char *const invalid[] = {
        "",
        "tap",
        "PRESS",
        "press extra",
        "release ",
        "status extra",
    };
    reset_fake_button();
    simulated_pressed = true;
    const unsigned int sets_before = set_calls;
    const unsigned int gets_before = get_calls;
    for (size_t index = 0U;
         index < sizeof(invalid) / sizeof(invalid[0]);
         ++index) {
        captured_output_t output = {0};
        assert(run_command(invalid[index], &output) == 1);
        assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_WARNING);
        assert(strcmp(
            output.text,
            "usage: button press|release|status|commands|bindings|defaults|bind <single|double|hold2|hold6|hold10> <command>\n") == 0);
    }
    assert(simulated_pressed);
    assert(set_calls == sets_before && get_calls == gets_before);
}

static void test_component_errors_are_visible(void)
{
    reset_fake_button();
    simulated_pressed = true;
    captured_output_t output = {0};
    set_result = ESP_FAIL;
    assert(run_command("release", &output) == 1);
    assert(simulated_pressed);
    assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_ERROR);
    assert(strcmp(output.text, "button: update failed: ESP_FAIL\n") == 0);
    set_result = ESP_OK;

    output = (captured_output_t) {0};
    get_result = ESP_FAIL;
    assert(run_command("status", &output) == 1);
    assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_ERROR);
    assert(strcmp(output.text, "button: status failed: ESP_FAIL\n") == 0);
    get_result = ESP_OK;
}

int main(void)
{
    test_press_release_and_status_are_idempotent();
    test_invalid_arguments_do_not_change_state();
    test_component_errors_are_visible();
    puts("Debug shell button tests passed");
    return 0;
}
