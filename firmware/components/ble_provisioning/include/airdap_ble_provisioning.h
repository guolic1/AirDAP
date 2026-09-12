#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads persistent bindings and starts GPIO0 monitoring; default hold2 opens BLE. */
esp_err_t airdap_ble_provisioning_start(void);

#ifdef __cplusplus
}
#endif
