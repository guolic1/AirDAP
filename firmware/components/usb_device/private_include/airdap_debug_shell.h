#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t airdap_debug_shell_start(void);
void airdap_debug_shell_disconnected(void);
void airdap_debug_shell_tx_complete(uint32_t sent_bytes);
