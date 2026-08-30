#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_debug_shell_input.h"

static const char prompt[] = "airdap> ";

static void write_text(
    const airdap_debug_shell_input_callbacks_t *callbacks,
    const char *text)
{
    callbacks->write(text, strlen(text), callbacks->context);
}

static void finish_line(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    write_text(callbacks, "\n");
    if (input->discarding) {
        write_text(callbacks, "error: command line too long\n");
    } else if (input->length > 0U) {
        input->line[input->length] = '\0';
        callbacks->execute(input->line, callbacks->context);
    }

    input->length = 0U;
    input->discarding = false;
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
            input->length = 0U;
            input->discarding = false;
            write_text(callbacks, "^C\n");
            write_text(callbacks, prompt);
            continue;
        }

        if (input->discarding) {
            continue;
        }

        if (byte == 0x08U || byte == 0x7FU) {
            if (input->length > 0U) {
                --input->length;
                write_text(callbacks, "\b \b");
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

        input->line[input->length] = (char) byte;
        ++input->length;
        callbacks->write((const char *) &byte, 1U, callbacks->context);
    }
}
