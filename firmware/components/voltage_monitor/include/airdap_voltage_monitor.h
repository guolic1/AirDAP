#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t target_mv;
    uint32_t usb_vbus_mv;
} airdap_voltage_reading_t;

/* Initialize calibrated ADC1 oneshot acquisition for both board dividers. */
esp_err_t airdap_voltage_monitor_init(void);

/* Read averaged, calibrated voltages at the original (pre-divider) nets. */
esp_err_t airdap_voltage_monitor_read(airdap_voltage_reading_t *reading);

#ifdef __cplusplus
}
#endif
