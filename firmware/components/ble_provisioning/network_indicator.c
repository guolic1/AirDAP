#include "airdap_network_indicator.h"

enum {
    CONNECTING_HALF_PERIOD_MS = 500,
    PROVISIONING_HALF_PERIOD_MS = 100,
};

void airdap_network_indicator_step(
    airdap_network_indicator_t *indicator,
    const airdap_mode_snapshot_t *mode,
    bool network_data_enabled,
    uint32_t elapsed_ms)
{
    airdap_network_indicator_pattern_t pattern = AIRDAP_NETWORK_INDICATOR_OFF;
    if (mode->provisioning == AIRDAP_PROVISIONING_ACTIVE) {
        pattern = AIRDAP_NETWORK_INDICATOR_PROVISIONING;
    } else if (network_data_enabled) {
        if (mode->wifi == AIRDAP_WIFI_ONLINE) {
            pattern = AIRDAP_NETWORK_INDICATOR_ON;
        } else if (mode->wifi == AIRDAP_WIFI_CONNECTING ||
                   mode->wifi == AIRDAP_WIFI_DISCONNECTED) {
            pattern = AIRDAP_NETWORK_INDICATOR_CONNECTING;
        }
    }

    const uint32_t half_period = pattern == AIRDAP_NETWORK_INDICATOR_CONNECTING
        ? CONNECTING_HALF_PERIOD_MS
        : pattern == AIRDAP_NETWORK_INDICATOR_PROVISIONING
            ? PROVISIONING_HALF_PERIOD_MS : 0U;
    if (pattern != indicator->pattern || half_period == 0U) {
        indicator->phase_ms = 0U;
    } else {
        const uint32_t period = half_period * 2U;
        indicator->phase_ms = (indicator->phase_ms + elapsed_ms % period) % period;
    }
    indicator->pattern = pattern;
    indicator->network_on = pattern == AIRDAP_NETWORK_INDICATOR_ON ||
        (half_period != 0U && indicator->phase_ms < half_period);
}
