#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_wifi_manager.h"

enum {
    AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE = 128,
};

typedef enum {
    AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW = 0,
    AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED,
    AIRDAP_DEBUG_SHELL_WIFI_STYLE_GREEN,
} airdap_debug_shell_wifi_style_t;

typedef struct {
    uint8_t ssid[AIRDAP_WIFI_SSID_MAX_LENGTH];
    uint8_t ssid_length;
    uint8_t input_stage;
} airdap_debug_shell_wifi_session_t;

void airdap_debug_shell_wifi_session_init(
    airdap_debug_shell_wifi_session_t *session);
void airdap_debug_shell_wifi_cancel(
    airdap_debug_shell_wifi_session_t *session);
bool airdap_debug_shell_wifi_input_pending(
    const airdap_debug_shell_wifi_session_t *session);
bool airdap_debug_shell_wifi_input_is_secret(
    const airdap_debug_shell_wifi_session_t *session);
const char *airdap_debug_shell_wifi_input_prompt(
    const airdap_debug_shell_wifi_session_t *session);

int airdap_debug_shell_wifi_execute(
    airdap_debug_shell_wifi_session_t *session,
    const char *arguments,
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style);
int airdap_debug_shell_wifi_submit(
    airdap_debug_shell_wifi_session_t *session,
    const char *line,
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style);
