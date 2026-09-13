#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Caller owns the allocated snapshot and releases it on scan/session change. */
esp_err_t airdap_debug_shell_wifi_scan(void **records, uint16_t *count);
bool airdap_debug_shell_wifi_scan_format(const void *records, unsigned index,
    char *output, size_t output_size);
