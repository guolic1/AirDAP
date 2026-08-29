#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_input.h"

enum {
    OUTPUT_CAPACITY = 1024,
    EXECUTED_CAPACITY = 8,
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
        0x01, 'h', 'e', 0x1B, 'l', 'p', '\n',
    };

    airdap_debug_shell_input_init(&input);
    airdap_debug_shell_input_consume(&input, bytes, sizeof(bytes), &callbacks);

    assert(capture.executed_count == 2U);
    assert(strcmp(capture.executed[0], "status") == 0);
    assert(strcmp(capture.executed[1], "help") == 0);
    assert(strstr(capture.output, "\b \b") != NULL);
    assert(strstr(capture.output, "^C\nairdap> ") != NULL);
}

static void test_overflow_discards_and_recovers(void)
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
    consume_text(&input, "\x03restart\n", &callbacks);
    consume_text(&input, "help\n", &callbacks);

    assert(capture.executed_count == 1U);
    assert(strcmp(capture.executed[0], "help") == 0);
    assert(strstr(capture.output, "error: command line too long\n") != NULL);
    assert(strstr(capture.output, "^C") == NULL);
}

int main(void)
{
    test_split_line_and_crlf();
    test_edit_cancel_and_control_filter();
    test_overflow_discards_and_recovers();

    puts("Debug shell input tests passed");
    return 0;
}
