#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the GPIO0 monitor. BLE remains off until a three-second press. */
esp_err_t airdap_ble_provisioning_start(void);

#ifdef __cplusplus
}
#endif
