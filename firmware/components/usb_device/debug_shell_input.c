#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_debug_shell_input.h"

static const char prompt[] = "airdap> ";

enum {
    ESCAPE_STATE_NONE = 0,
    ESCAPE_STATE_STARTED,
    ESCAPE_STATE_CSI,
    ESCAPE_STATE_SS3,
};

static void write_text(
    const airdap_debug_shell_input_callbacks_t *callbacks,
    const char *text)
{
    callbacks->write(text, strlen(text), callbacks->context);
}

static void reset_editor(airdap_debug_shell_input_t *input)
{
    input->line[0] = '\0';
    input->draft[0] = '\0';
    input->length = 0U;
    input->cursor = 0U;
    input->history_offset = 0U;
    input->draft_length = 0U;
    input->draft_cursor = 0U;
    input->escape_parameter = 0U;
    input->escape_state = ESCAPE_STATE_NONE;
    input->escape_has_parameter = false;
    input->escape_invalid = false;
    input->discarding = false;
}

static void leave_history_navigation(airdap_debug_shell_input_t *input)
{
    input->history_offset = 0U;
    input->draft_length = 0U;
    input->draft_cursor = 0U;
    input->draft[0] = '\0';
}

static void add_history(airdap_debug_shell_input_t *input)
{
    if (input->history_count > 0U) {
        const size_t latest =
            (input->history_next + AIRDAP_DEBUG_SHELL_HISTORY_DEPTH - 1U) %
            AIRDAP_DEBUG_SHELL_HISTORY_DEPTH;
        if (strcmp(input->history[latest], input->line) == 0) {
            return;
        }
    }

    memcpy(
        input->history[input->history_next],
        input->line,
        input->length + 1U);
    input->history_next =
        (input->history_next + 1U) % AIRDAP_DEBUG_SHELL_HISTORY_DEPTH;
    if (input->history_count < AIRDAP_DEBUG_SHELL_HISTORY_DEPTH) {
        ++input->history_count;
    }
}

static void write_repeated(
    char value,
    size_t count,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    char output[AIRDAP_DEBUG_SHELL_LINE_CAPACITY + sizeof(prompt)];

    if (count == 0U) {
        return;
    }
    memset(output, value, count);
    callbacks->write(output, count, callbacks->context);
}

static void clear_rendered_line(
    size_t line_length,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    write_text(callbacks, "\r");
    write_repeated(
        ' ',
        sizeof(prompt) - 1U + line_length,
        callbacks);
    write_text(callbacks, "\r");
}

static void render_line(
    const airdap_debug_shell_input_t *input,
    size_t erased_line_length,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    clear_rendered_line(erased_line_length, callbacks);
    write_text(callbacks, prompt);
    if (input->length > 0U) {
        callbacks->write(input->line, input->length, callbacks->context);
    }
    write_repeated('\b', input->length - input->cursor, callbacks);
}

static void replace_line(
    airdap_debug_shell_input_t *input,
    const char *replacement,
    size_t replacement_length,
    size_t replacement_cursor,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    const size_t previous_length = input->length;

    if (replacement_length >= sizeof(input->line) ||
        replacement_cursor > replacement_length) {
        return;
    }

    memcpy(input->line, replacement, replacement_length);
    input->line[replacement_length] = '\0';
    input->length = replacement_length;
    input->cursor = replacement_cursor;
    render_line(input, previous_length, callbacks);
}

static bool history_matches(
    const airdap_debug_shell_input_t *input,
    size_t history_offset)
{
    const size_t history_index =
        (input->history_next + AIRDAP_DEBUG_SHELL_HISTORY_DEPTH -
         history_offset) % AIRDAP_DEBUG_SHELL_HISTORY_DEPTH;
    return strncmp(
        input->history[history_index],
        input->draft,
        input->draft_length) == 0;
}

static void recall_history(
    airdap_debug_shell_input_t *input,
    bool older,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input->history_count == 0U) {
        return;
    }
    if (!older && input->history_offset == 0U) {
        return;
    }

    if (input->history_offset == 0U) {
        memcpy(input->draft, input->line, input->length + 1U);
        input->draft_length = input->length;
        input->draft_cursor = input->cursor;
    }

    if (older) {
        size_t candidate = input->history_offset + 1U;
        while (candidate <= input->history_count &&
               !history_matches(input, candidate)) {
            ++candidate;
        }
        if (candidate > input->history_count) {
            return;
        }
        input->history_offset = candidate;
    } else {
        size_t candidate = input->history_offset - 1U;
        while (candidate > 0U && !history_matches(input, candidate)) {
            --candidate;
        }
        input->history_offset = candidate;
    }

    if (input->history_offset == 0U) {
        replace_line(
            input,
            input->draft,
            input->draft_length,
            input->draft_cursor,
            callbacks);
        return;
    }

    const size_t history_index =
        (input->history_next + AIRDAP_DEBUG_SHELL_HISTORY_DEPTH -
         input->history_offset) % AIRDAP_DEBUG_SHELL_HISTORY_DEPTH;
    const size_t history_length = strlen(input->history[history_index]);
    replace_line(
        input,
        input->history[history_index],
        history_length,
        history_length,
        callbacks);
}

static void complete_command(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (callbacks->complete == NULL || input->length == 0U ||
        input->cursor != input->length ||
        memchr(input->line, ' ', input->length) != NULL) {
        return;
    }

    input->line[input->length] = '\0';
    const char *completion = callbacks->complete(
        input->line,
        callbacks->context);
    if (completion == NULL) {
        return;
    }

    const size_t completion_length = strlen(completion);
    if (completion_length < input->length ||
        completion_length >= sizeof(input->line) ||
        strncmp(completion, input->line, input->length) != 0) {
        return;
    }

    leave_history_navigation(input);
    const size_t suffix_length = completion_length - input->length;
    if (suffix_length > 0U) {
        memcpy(input->line + input->length, completion + input->length, suffix_length);
        callbacks->write(
            completion + input->length,
            suffix_length,
            callbacks->context);
        input->length = completion_length;
        input->cursor = input->length;
    }
    if (input->length + 1U < sizeof(input->line)) {
        input->line[input->length] = ' ';
        ++input->length;
        input->cursor = input->length;
        callbacks->write(" ", 1U, callbacks->context);
    }
    input->line[input->length] = '\0';
}

static void reset_escape_sequence(airdap_debug_shell_input_t *input)
{
    input->escape_parameter = 0U;
    input->escape_state = ESCAPE_STATE_NONE;
    input->escape_has_parameter = false;
    input->escape_invalid = false;
}

static void move_cursor_left(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input->cursor == 0U) {
        return;
    }
    --input->cursor;
    write_text(callbacks, "\b");
}

static void move_cursor_right(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input->cursor >= input->length) {
        return;
    }
    callbacks->write(
        input->line + input->cursor,
        1U,
        callbacks->context);
    ++input->cursor;
}

static void move_cursor_home(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    write_repeated('\b', input->cursor, callbacks);
    input->cursor = 0U;
}

static void move_cursor_end(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input->cursor < input->length) {
        callbacks->write(
            input->line + input->cursor,
            input->length - input->cursor,
            callbacks->context);
        input->cursor = input->length;
    }
}

static void delete_at_cursor(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input->cursor >= input->length) {
        return;
    }

    const size_t previous_length = input->length;
    leave_history_navigation(input);
    memmove(
        input->line + input->cursor,
        input->line + input->cursor + 1U,
        input->length - input->cursor);
    --input->length;
    render_line(input, previous_length, callbacks);
}

static void handle_navigation_key(
    airdap_debug_shell_input_t *input,
    uint8_t key,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    switch (key) {
    case 'A':
        recall_history(input, true, callbacks);
        break;
    case 'B':
        recall_history(input, false, callbacks);
        break;
    case 'C':
        move_cursor_right(input, callbacks);
        break;
    case 'D':
        move_cursor_left(input, callbacks);
        break;
    case 'H':
        move_cursor_home(input, callbacks);
        break;
    case 'F':
        move_cursor_end(input, callbacks);
        break;
    default:
        break;
    }
}

static void handle_csi_final(
    airdap_debug_shell_input_t *input,
    uint8_t byte,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input->escape_invalid) {
        return;
    }
    if (!input->escape_has_parameter) {
        handle_navigation_key(input, byte, callbacks);
        return;
    }
    if (byte != '~') {
        return;
    }

    switch (input->escape_parameter) {
    case 1U:
    case 7U:
        move_cursor_home(input, callbacks);
        break;
    case 3U:
        delete_at_cursor(input, callbacks);
        break;
    case 4U:
    case 8U:
        move_cursor_end(input, callbacks);
        break;
    default:
        break;
    }
}

static bool consume_escape_sequence(
    airdap_debug_shell_input_t *input,
    uint8_t byte,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input->escape_state == ESCAPE_STATE_NONE) {
        if (byte != 0x1BU) {
            return false;
        }
        input->escape_state = ESCAPE_STATE_STARTED;
        return true;
    }

    if (input->escape_state == ESCAPE_STATE_STARTED) {
        if (byte == '[') {
            input->escape_state = ESCAPE_STATE_CSI;
        } else if (byte == 'O') {
            input->escape_state = ESCAPE_STATE_SS3;
        } else {
            reset_escape_sequence(input);
        }
        return true;
    }

    if (input->escape_state == ESCAPE_STATE_SS3) {
        handle_navigation_key(input, byte, callbacks);
        reset_escape_sequence(input);
        return true;
    }

    if (byte >= '0' && byte <= '9') {
        const uint16_t digit = (uint16_t) (byte - '0');
        input->escape_has_parameter = true;
        if (input->escape_parameter > (UINT16_MAX - digit) / 10U) {
            input->escape_invalid = true;
        } else {
            input->escape_parameter =
                (uint16_t) (input->escape_parameter * 10U + digit);
        }
    } else if (byte >= 0x40U && byte <= 0x7EU) {
        handle_csi_final(input, byte, callbacks);
        reset_escape_sequence(input);
    } else if (byte >= 0x20U && byte <= 0x3FU) {
        input->escape_invalid = true;
    } else {
        reset_escape_sequence(input);
    }
    return true;
}

static void finish_line(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    move_cursor_end(input, callbacks);
    write_text(callbacks, "\n");
    if (input->discarding) {
        write_text(callbacks, "error: command line too long\n");
    } else if (input->length > 0U) {
        input->line[input->length] = '\0';
        add_history(input);
        callbacks->execute(input->line, callbacks->context);
    }

    reset_editor(input);
    write_text(callbacks, prompt);
}

void airdap_debug_shell_input_init(airdap_debug_shell_input_t *input)
{
    if (input == NULL) {
        return;
    }
    memset(input, 0, sizeof(*input));
}

void airdap_debug_shell_input_consume(
    airdap_debug_shell_input_t *input,
    const uint8_t *data,
    size_t length,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input == NULL || data == NULL || callbacks == NULL ||
        callbacks->write == NULL || callbacks->execute == NULL) {
        return;
    }

    for (size_t index = 0U; index < length; ++index) {
        const uint8_t byte = data[index];

        if (input->skip_lf) {
            input->skip_lf = false;
            if (byte == '\n') {
                continue;
            }
        }

        if (byte == '\r' || byte == '\n') {
            finish_line(input, callbacks);
            input->skip_lf = byte == '\r';
            continue;
        }

        if (byte == 0x03U) {
            move_cursor_end(input, callbacks);
            reset_editor(input);
            write_text(callbacks, "^C\n");
            write_text(callbacks, prompt);
            continue;
        }

        if (input->discarding) {
            continue;
        }

        if (consume_escape_sequence(input, byte, callbacks)) {
            continue;
        }

        if (byte == '\t') {
            complete_command(input, callbacks);
            continue;
        }

        if (byte == 0x08U || byte == 0x7FU) {
            if (input->cursor > 0U) {
                const size_t previous_length = input->length;
                leave_history_navigation(input);
                if (input->cursor == input->length) {
                    --input->cursor;
                    --input->length;
                    input->line[input->length] = '\0';
                    write_text(callbacks, "\b \b");
                    continue;
                }
                memmove(
                    input->line + input->cursor - 1U,
                    input->line + input->cursor,
                    input->length - input->cursor + 1U);
                --input->cursor;
                --input->length;
                render_line(input, previous_length, callbacks);
            }
            continue;
        }

        if (byte < 0x20U || byte > 0x7EU) {
            continue;
        }

        if (input->length + 1U >= sizeof(input->line)) {
            input->discarding = true;
            continue;
        }

        leave_history_navigation(input);
        if (input->cursor < input->length) {
            const size_t previous_length = input->length;
            memmove(
                input->line + input->cursor + 1U,
                input->line + input->cursor,
                input->length - input->cursor + 1U);
            input->line[input->cursor] = (char) byte;
            ++input->cursor;
            ++input->length;
            render_line(input, previous_length, callbacks);
            continue;
        }

        input->line[input->cursor] = (char) byte;
        ++input->cursor;
        ++input->length;
        input->line[input->length] = '\0';
        callbacks->write((const char *) &byte, 1U, callbacks->context);
    }
}

void airdap_debug_shell_input_write_background(
    const airdap_debug_shell_input_t *input,
    const char *data,
    size_t length,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    if (input == NULL || data == NULL || length == 0U ||
        callbacks == NULL || callbacks->write == NULL) {
        return;
    }

    clear_rendered_line(input->length, callbacks);
    callbacks->write(data, length, callbacks->context);
    if (data[length - 1U] != '\n') {
        write_text(callbacks, "\n");
    }
    write_text(callbacks, prompt);
    if (input->length > 0U) {
        callbacks->write(input->line, input->length, callbacks->context);
    }
    write_repeated('\b', input->length - input->cursor, callbacks);
}
