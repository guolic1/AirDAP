#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t target_mv;
    uint32_t usb_vbus_mv;
} airdap_voltage_reading_t;

esp_err_t airdap_voltage_monitor_init(void);
esp_err_t airdap_voltage_monitor_read(airdap_voltage_reading_t *reading);
