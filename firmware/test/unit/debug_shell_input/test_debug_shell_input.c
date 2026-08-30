#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_input.h"

enum {
    OUTPUT_CAPACITY = 1024,
    EXECUTED_CAPACITY = 16,
};

typedef struct {
    char output[OUTPUT_CAPACITY];
    size_t output_length;
    char executed[EXECUTED_CAPACITY][AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
    size_t executed_count;
} capture_t;

static void capture_write(const char *data, size_t length, void *context)
{
    capture_t *capture = context;
    assert(capture->output_length + length < sizeof(capture->output));
    memcpy(capture->output + capture->output_length, data, length);
    capture->output_length += length;
    capture->output[capture->output_length] = '\0';
}

static void capture_execute(const char *line, void *context)
{
    capture_t *capture = context;
    assert(capture->executed_count < EXECUTED_CAPACITY);
    assert(strlen(line) < sizeof(capture->executed[0]));
    strcpy(capture->executed[capture->executed_count], line);
    ++capture->executed_count;
}

static const char *complete_command(const char *prefix, void *context)
{
    static const char *const commands[] = {
        "help",
        "status",
        "restart",
    };
    const size_t prefix_length = strlen(prefix);
    const char *match = NULL;

    (void) context;
    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        if (strncmp(commands[index], prefix, prefix_length) != 0) {
            continue;
        }
        if (match != NULL) {
            return NULL;
        }
        match = commands[index];
    }
    return match;
}

static airdap_debug_shell_input_callbacks_t make_callbacks(capture_t *capture)
{
    return (airdap_debug_shell_input_callbacks_t) {
        .write = capture_write,
        .execute = capture_execute,
        .context = capture,
    };
}

static void consume_text(
    airdap_debug_shell_input_t *input,
    const char *text,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    airdap_debug_shell_input_consume(
        input,
        (const uint8_t *) text,
        strlen(text),
        callbacks);
}

static void test_split_line_and_crlf(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "sta", &callbacks);
    consume_text(&input, "tus\r", &callbacks);
    consume_text(&input, "\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "status") == 0);
    assert(strcmp(capture.output, "status\nairdap> ") == 0);
}

static void test_edit_cancel_and_control_filter(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);
    static const uint8_t bytes[] = {
        's', 't', 'x', 0x08, 'a', 't', 'u', 's', '\n',
        'b', 'a', 'd', 0x03,
        0x01, 'h', 'e', 0x02, 'l', 'p', '\n',
    };

    airdap_debug_shell_input_init(&input);
    airdap_debug_shell_input_consume(&input, bytes, sizeof(bytes), &callbacks);

    assert(capture.executed_count == 2U);
    assert(strcmp(capture.executed[0], "status") == 0);
    assert(strcmp(capture.executed[1], "help") == 0);
    assert(strstr(capture.output, "\b \b") != NULL);
    assert(strstr(capture.output, "^C\nairdap> ") != NULL);
}

static void test_overflow_newline_reports_and_recovers(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);
    uint8_t oversized[AIRDAP_DEBUG_SHELL_LINE_CAPACITY];

    memset(oversized, 'a', sizeof(oversized));

    airdap_debug_shell_input_init(&input);
    airdap_debug_shell_input_consume(
        &input,
        oversized,
        sizeof(oversized),
        &callbacks);
    consume_text(&input, "\n", &callbacks);
    consume_text(&input, "help\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "help") == 0);
    assert(strstr(capture.output, "error: command line too long\n") != NULL);
}

static void test_ctrl_c_cancels_overflow_and_recovers(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);
    uint8_t oversized[AIRDAP_DEBUG_SHELL_LINE_CAPACITY];

    memset(oversized, 'a', sizeof(oversized));

    airdap_debug_shell_input_init(&input);
    airdap_debug_shell_input_consume(
        &input,
        oversized,
        sizeof(oversized),
        &callbacks);
    consume_text(&input, "\x03help\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "help") == 0);
    assert(strstr(capture.output, "^C\nairdap> ") != NULL);
    assert(strstr(capture.output, "error: command line too long\n") == NULL);
}

static void test_tab_completes_unique_command(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    callbacks.complete = complete_command;
    airdap_debug_shell_input_init(&input);
    consume_text(&input, "st\t\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "status ") == 0);
    assert(strcmp(capture.output, "status \nairdap> ") == 0);
}

static void test_up_recalls_latest_command_across_split_escape_sequence(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "help\n", &callbacks);
    consume_text(&input, "\x1b", &callbacks);
    consume_text(&input, "[A\n", &callbacks);

    assert(capture.executed_count == 2U);
    assert(strcmp(capture.executed[1], "help") == 0);
}

static void test_down_restores_unsubmitted_draft(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "help\nstatus\nst\x1b[D", &callbacks);
    consume_text(&input, "\x1b[A\x1b[Ba\n", &callbacks);

    assert(capture.executed_count == 3U);
    assert(strcmp(capture.executed[2], "sat") == 0);
}

static void test_history_keeps_last_eight_commands(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);
    char command[16];

    airdap_debug_shell_input_init(&input);
    for (size_t index = 0U; index < 9U; ++index) {
        const int length = snprintf(command, sizeof(command), "command%zu\n", index);
        assert(length > 0 && (size_t) length < sizeof(command));
        consume_text(&input, command, &callbacks);
    }
    for (size_t index = 0U; index < 9U; ++index) {
        consume_text(&input, "\x1b[A", &callbacks);
    }
    consume_text(&input, "\n", &callbacks);

    assert(capture.executed_count == 10U);
    assert(strcmp(capture.executed[9], "command1") == 0);
}

static void test_unsupported_cursor_sequence_is_not_inserted(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "he\x1b[5~lp\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "help") == 0);
}

static void test_history_is_cleared_with_session_input_state(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "help\n", &callbacks);
    airdap_debug_shell_input_init(&input);
    consume_text(&input, "\x1b[A\n", &callbacks);

    assert(capture.executed_count == 1U);
}

static void test_cursor_insertion_and_right_arrow(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "helo\x1b[Dl\x1b[C!\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "hello!") == 0);
}

static void test_home_end_and_delete(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(
        &input,
        "xhello\x1b[H\x1b[3~\x1b[F!\x1b[D\x1b[3~\n",
        &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "hello") == 0);
}

static void test_backspace_edits_at_cursor(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "helplo\x1b[D\x1b[D\x7f\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "hello") == 0);
}

static void test_history_navigation_filters_by_draft_prefix(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);

    airdap_debug_shell_input_init(&input);
    consume_text(
        &input,
        "status one\nhelp\nstatus two\nrestart\nstatus",
        &callbacks);
    consume_text(&input, "\x1b[A\x1b[A\n", &callbacks);

    assert(capture.executed_count == 5U);
    assert(strcmp(capture.executed[4], "status one") == 0);
}

static void test_background_output_restores_line_and_cursor(void)
{
    airdap_debug_shell_input_t input;
    capture_t capture = {0};
    const airdap_debug_shell_input_callbacks_t callbacks = make_callbacks(&capture);
    static const char log_line[] = "background log\n";

    airdap_debug_shell_input_init(&input);
    consume_text(&input, "helo\x1b[D", &callbacks);
    airdap_debug_shell_input_write_background(
        &input,
        log_line,
        sizeof(log_line) - 1U,
        &callbacks);
    consume_text(&input, "l\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "hello") == 0);
    assert(strstr(capture.output, "background log\nairdap> helo\b") != NULL);
}

int main(void)
{
    test_split_line_and_crlf();
    test_edit_cancel_and_control_filter();
    test_overflow_newline_reports_and_recovers();
    test_ctrl_c_cancels_overflow_and_recovers();
    test_tab_completes_unique_command();
    test_up_recalls_latest_command_across_split_escape_sequence();
    test_down_restores_unsubmitted_draft();
    test_history_keeps_last_eight_commands();
    test_unsupported_cursor_sequence_is_not_inserted();
    test_history_is_cleared_with_session_input_state();
    test_cursor_insertion_and_right_arrow();
    test_home_end_and_delete();
    test_backspace_edits_at_cursor();
    test_history_navigation_filters_by_draft_prefix();
    test_background_output_restores_line_and_cursor();

    puts("Debug shell input tests passed");
    return 0;
}
