#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    AIRDAP_DEBUG_SHELL_LINE_CAPACITY = 128,
    AIRDAP_DEBUG_SHELL_HISTORY_DEPTH = 8,
};

typedef struct {
    char line[AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
    char history[AIRDAP_DEBUG_SHELL_HISTORY_DEPTH][AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
    char draft[AIRDAP_DEBUG_SHELL_LINE_CAPACITY];
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
} airdap_debug_shell_input_t;

typedef struct {
    void (*write)(const char *data, size_t length, void *context);
    void (*execute)(const char *line, void *context);
    const char *(*complete)(const char *prefix, size_t match_index, void *context);
    void *context;
} airdap_debug_shell_input_callbacks_t;

void airdap_debug_shell_input_init(airdap_debug_shell_input_t *input);
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
