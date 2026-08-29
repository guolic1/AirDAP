#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reacquire all target-facing pins in their non-invasive startup state. */
esp_err_t airdap_board_init_safe(void);

/*
 * Release or pull low the shared TPS2116 ST / TPS22919 ON network.
 * "Allowed" means the external open-drain status circuit may enable target
 * power; it does not guarantee that USB is selected or that VTref is present.
 */
esp_err_t airdap_target_power_set_allowed(bool allowed);
esp_err_t airdap_target_power_get_active(bool *active);

/* GPIO41 drives an inverting transistor: high asserts target nRESET. */
esp_err_t airdap_target_reset_set_asserted(bool asserted);

#ifdef __cplusplus
}
#endif
