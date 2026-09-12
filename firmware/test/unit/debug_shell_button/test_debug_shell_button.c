#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "airdap_button_simulation.h"
#include "airdap_debug_shell_button.h"

typedef struct { char text[256]; airdap_debug_shell_style_t style; } output_t;
static airdap_button_gesture_t active = AIRDAP_BUTTON_GESTURE_COUNT;
static unsigned requests, reads;
static esp_err_t request_error, read_error;
esp_err_t airdap_button_simulate(airdap_button_gesture_t gesture)
{
    ++requests;
    if (request_error == ESP_OK) active = gesture;
    return request_error;
}
esp_err_t airdap_button_simulation_get(airdap_button_gesture_t *gesture)
{
    ++reads;
    if (read_error == ESP_OK) *gesture = active;
    return read_error;
}
static void capture(airdap_debug_shell_style_t style, const char *format, va_list args, void *context)
{
    output_t *output = context;
    output->style = style;
    const size_t used = strlen(output->text);
    vsnprintf(output->text + used, sizeof(output->text) - used, format, args);
}
static int run(const char *arguments, output_t *output)
{
    *output = (output_t) {0};
    const airdap_debug_shell_invocation_t invocation = {.vprintf = capture, .output_context = output};
    return airdap_debug_shell_button_command(arguments, &invocation, NULL);
}
int main(void)
{
    output_t output;
    for (int g = 0; g < AIRDAP_BUTTON_GESTURE_COUNT; ++g) {
        char command[40], expected[80];
        const char *name = airdap_button_gesture_name((airdap_button_gesture_t) g);
        snprintf(command, sizeof(command), "simulate %s", name);
        assert(run(command, &output) == 0 && active == (airdap_button_gesture_t) g);
        snprintf(expected, sizeof(expected), "button: simulation=%s accepted\n", name);
        assert(strcmp(output.text, expected) == 0 && output.style == AIRDAP_DEBUG_SHELL_STYLE_SUCCESS);
        assert(run("status", &output) == 0);
        snprintf(expected, sizeof(expected), "button: simulation=%s\n", name);
        assert(strcmp(output.text, expected) == 0);
    }
    const char *invalid[] = {NULL, "", "press", "release", "simulate", "simulate hold3", "simulate HOLD2",
        "simulate single extra", "simulate single ", "simulate  single", "status extra"};
    const unsigned before_requests = requests, before_reads = reads;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(run(invalid[i], &output) == 1);
        assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_WARNING && strstr(output.text, "usage: button simulate"));
    }
    assert(requests == before_requests && reads == before_reads);
    request_error = ESP_FAIL;
    assert(run("simulate single", &output) == 1 && active == AIRDAP_BUTTON_GESTURE_HOLD10);
    assert(output.style == AIRDAP_DEBUG_SHELL_STYLE_ERROR && strstr(output.text, "simulate failed: ESP_FAIL"));
    read_error = ESP_FAIL;
    assert(run("status", &output) == 1 && strstr(output.text, "status failed: ESP_FAIL"));
    read_error = ESP_OK;
    active = AIRDAP_BUTTON_GESTURE_COUNT;
    assert(run("status", &output) == 0 && strcmp(output.text, "button: simulation=idle\n") == 0);
    puts("Debug shell button simulation commands passed");
    return 0;
}
