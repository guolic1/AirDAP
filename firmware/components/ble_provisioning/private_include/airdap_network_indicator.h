#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airdap_mode_state.h"

typedef enum {
    AIRDAP_NETWORK_INDICATOR_OFF = 0,
    AIRDAP_NETWORK_INDICATOR_ON,
    AIRDAP_NETWORK_INDICATOR_CONNECTING,
    AIRDAP_NETWORK_INDICATOR_PROVISIONING,
} airdap_network_indicator_pattern_t;

typedef struct {
    airdap_network_indicator_pattern_t pattern;
    uint32_t phase_ms;
    bool network_on;
} airdap_network_indicator_t;

/* Zero-initialize once. Pattern changes start lit; equal patterns preserve
 * phase across Wi-Fi retry transitions. The caller owns GPIO writes. */
void airdap_network_indicator_step(
    airdap_network_indicator_t *indicator,
    const airdap_mode_snapshot_t *mode,
    bool network_data_enabled,
    uint32_t elapsed_ms);
