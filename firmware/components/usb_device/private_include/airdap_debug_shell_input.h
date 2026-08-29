#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    AIRDAP_DEBUG_SHELL_LINE_CAPACITY = 128,
};

typedef struct {
    char line[AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
    size_t length;
    bool discarding;
    bool skip_lf;
} airdap_debug_shell_input_t;

typedef struct {
    void (*write)(const char *data, size_t length, void *context);
    void (*execute)(const char *line, void *context);
    void *context;
} airdap_debug_shell_input_callbacks_t;

void airdap_debug_shell_input_init(airdap_debug_shell_input_t *input);
void airdap_debug_shell_input_consume(
    airdap_debug_shell_input_t *input,
    const uint8_t *data,
    size_t length,
    const airdap_debug_shell_input_callbacks_t *callbacks);
