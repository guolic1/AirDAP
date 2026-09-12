#pragma once
#include "airdap_button_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Queue one complete gesture. Requires the running, idle button monitor.
 * Acceptance is asynchronous; physical input or read failure cancels simulation. */
esp_err_t airdap_button_simulate(airdap_button_gesture_t gesture);
/* COUNT means no queued/running simulation, not necessarily an idle physical key. */
esp_err_t airdap_button_simulation_get(airdap_button_gesture_t *gesture);

#ifdef __cplusplus
}
#endif
