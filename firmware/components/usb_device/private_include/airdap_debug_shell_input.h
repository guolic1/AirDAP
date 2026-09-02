#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    AIRDAP_DEBUG_SHELL_LINE_CAPACITY = 128,
    AIRDAP_DEBUG_SHELL_HISTORY_DEPTH = 8,
    AIRDAP_DEBUG_SHELL_PROMPT_CAPACITY = 16,
};

typedef enum {
    AIRDAP_DEBUG_SHELL_INPUT_COMMAND = 0,
    AIRDAP_DEBUG_SHELL_INPUT_TEXT,
    AIRDAP_DEBUG_SHELL_INPUT_SECRET,
} airdap_debug_shell_input_mode_t;

typedef struct {
    char line[AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
    char history[AIRDAP_DEBUG_SHELL_HISTORY_DEPTH][AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
    char draft[AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
    char prompt[AIRDAP_DEBUG_SHELL_PROMPT_CAPACITY];
    size_t length;
    size_t cursor;
    size_t history_count;
    size_t history_next;
    size_t history_offset;
    size_t draft_length;
    size_t draft_cursor;
    uint16_t escape_parameter;
    uint8_t escape_state;
    bool escape_has_parameter;
    bool escape_invalid;
    bool discarding;
    bool skip_lf;
    airdap_debug_shell_input_mode_t mode;
} airdap_debug_shell_input_t;

typedef struct {
    void (*write)(const char *data, size_t length, void *context);
    void (*execute)(const char *line, void *context);
    void (*cancel)(void *context);
    const char *(*complete)(const char *prefix, size_t match_index, void *context);
    bool color_enabled;
    void *context;
} airdap_debug_shell_input_callbacks_t;

void airdap_debug_shell_input_init(airdap_debug_shell_input_t *input);
bool airdap_debug_shell_input_set_mode(
    airdap_debug_shell_input_t *input,
    airdap_debug_shell_input_mode_t mode,
    const char *prompt);
void airdap_debug_shell_input_consume(
    airdap_debug_shell_input_t *input,
    const uint8_t *data,
    size_t length,
    const airdap_debug_shell_input_callbacks_t *callbacks);
void airdap_debug_shell_input_write_background(
    const airdap_debug_shell_input_t *input,
    const char *data,
    size_t length,
    const airdap_debug_shell_input_callbacks_t *callbacks);
