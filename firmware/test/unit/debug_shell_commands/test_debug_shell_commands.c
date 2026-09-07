#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_commands.h"

typedef struct {
    char text[1024];
    size_t length;
    airdap_debug_shell_style_t style;
    void *session;
} captured_output_t;

static void capture_vprintf(
    airdap_debug_shell_style_t style,
    const char *format,
    va_list arguments,
    void *context)
{
    captured_output_t *captured = context;
    captured->style = style;
    const int formatted = vsnprintf(
        captured->text + captured->length,
        sizeof(captured->text) - captured->length,
        format,
        arguments);
    if (formatted > 0) {
        const size_t available =
            sizeof(captured->text) - captured->length;
        captured->length += (size_t) formatted < available
            ? (size_t) formatted
            : available - 1U;
    }
}

static int example_handler(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    captured_output_t *captured = context;
    captured->session = invocation->session;
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "arguments=%s",
        arguments);
    return 7;
}

static const airdap_debug_shell_command_t alpha_command = {
    .name = "alpha",
    .usage = "alpha [value]",
    .summary = "Run the alpha diagnostic",
    .details = "Reports the value received by the alpha diagnostic.",
    .handler = example_handler,
};

static const airdap_debug_shell_command_t beta_command = {
    .name = "beta-info",
    .usage = "beta-info",
    .summary = "Show beta information",
    .details = "Reports read-only beta information.",
    .handler = example_handler,
};

static void test_registers_commands_from_independent_sources(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);

    assert(airdap_debug_shell_command_register(&registry, &alpha_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED);
    assert(airdap_debug_shell_command_register(&registry, &beta_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED);
    assert(airdap_debug_shell_command_count(&registry) == 2U);
    assert(airdap_debug_shell_command_at(&registry, 0U) == &alpha_command);
    assert(airdap_debug_shell_command_at(&registry, 1U) == &beta_command);
    assert(airdap_debug_shell_command_at(&registry, 2U) == NULL);

    assert(airdap_debug_shell_command_find(&registry, "alpha", 5U) ==
        &alpha_command);
    assert(airdap_debug_shell_command_find(&registry, "alpha-more", 5U) ==
        &alpha_command);
    assert(airdap_debug_shell_command_find(&registry, "alph", 4U) == NULL);

    assert(strcmp(
        airdap_debug_shell_command_complete(&registry, "", 0U),
        "alpha") == 0);
    assert(strcmp(
        airdap_debug_shell_command_complete(&registry, "b", 0U),
        "beta-info") == 0);
    assert(airdap_debug_shell_command_complete(&registry, "b", 1U) == NULL);
}

static void test_rejects_invalid_duplicate_full_and_late_registration(void)
{
    static const airdap_debug_shell_command_t invalid_name = {
        .name = "bad name",
        .usage = "bad name",
        .summary = "Invalid",
        .details = "Invalid command name.",
        .handler = example_handler,
    };
    static const airdap_debug_shell_command_t missing_description = {
        .name = "missing",
        .usage = "missing",
        .summary = "Missing details",
        .details = NULL,
        .handler = example_handler,
    };
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);

    assert(airdap_debug_shell_command_register(NULL, &alpha_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_INVALID);
    assert(airdap_debug_shell_command_register(&registry, NULL) ==
        AIRDAP_DEBUG_SHELL_COMMAND_INVALID);
    assert(airdap_debug_shell_command_register(&registry, &invalid_name) ==
        AIRDAP_DEBUG_SHELL_COMMAND_INVALID);
    assert(airdap_debug_shell_command_register(
        &registry,
        &missing_description) == AIRDAP_DEBUG_SHELL_COMMAND_INVALID);
    assert(airdap_debug_shell_command_register(&registry, &alpha_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED);
    assert(airdap_debug_shell_command_register(&registry, &alpha_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_DUPLICATE);

    airdap_debug_shell_command_registry_freeze(&registry);
    assert(airdap_debug_shell_command_register(&registry, &beta_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_FROZEN);

    airdap_debug_shell_command_registry_init(&registry);
    for (size_t index = 0U;
         index < AIRDAP_DEBUG_SHELL_MAX_COMMANDS;
         ++index) {
        registry.entries[index] = &alpha_command;
    }
    registry.count = AIRDAP_DEBUG_SHELL_MAX_COMMANDS;
    assert(airdap_debug_shell_command_register(&registry, &beta_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_FULL);
}

static void test_invokes_handler_with_output_and_context(void)
{
    captured_output_t captured = {0};
    int session_marker = 0;
    airdap_debug_shell_invocation_t invocation = {
        .session = &session_marker,
        .vprintf = capture_vprintf,
        .output_context = &captured,
    };
    airdap_debug_shell_command_t command = alpha_command;
    command.context = &captured;

    assert(command.handler("42", &invocation, command.context) == 7);
    assert(captured.session == &session_marker);
    assert(captured.style == AIRDAP_DEBUG_SHELL_STYLE_SUCCESS);
    assert(strcmp(captured.text, "arguments=42") == 0);
}

static void test_formats_command_list_and_detailed_help(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_command_register(&registry, &alpha_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED);
    assert(airdap_debug_shell_command_register(&registry, &beta_command) ==
        AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED);

    captured_output_t captured = {0};
    airdap_debug_shell_invocation_t invocation = {
        .vprintf = capture_vprintf,
        .output_context = &captured,
    };
    assert(airdap_debug_shell_command_help(
        &registry,
        "",
        &invocation) == 0);
    assert(strcmp(
        captured.text,
        "alpha     Run the alpha diagnostic\n"
        "beta-info Show beta information\n") == 0);

    memset(&captured, 0, sizeof(captured));
    assert(airdap_debug_shell_command_help(
        &registry,
        "alpha",
        &invocation) == 0);
    assert(strcmp(
        captured.text,
        "usage: alpha [value]\n"
        "summary: Run the alpha diagnostic\n"
        "details: Reports the value received by the alpha diagnostic.\n") ==
        0);

    memset(&captured, 0, sizeof(captured));
    assert(airdap_debug_shell_command_help(
        &registry,
        "unknown",
        &invocation) == 1);
    assert(captured.style == AIRDAP_DEBUG_SHELL_STYLE_ERROR);
    assert(strcmp(captured.text, "help: command not found: unknown\n") == 0);
}

int main(void)
{
    test_registers_commands_from_independent_sources();
    test_rejects_invalid_duplicate_full_and_late_registration();
    test_invokes_handler_with_output_and_context();
    test_formats_command_list_and_detailed_help();

    puts("Debug shell command registry tests passed");
    return 0;
}
