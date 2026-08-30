#pragma once

#include <stdint.h>

#include "esp_err.h"

#define AIRDAP_SWD_DEFAULT_CLOCK_HZ 1000000U

esp_err_t airdap_swd_init(uint32_t clock_hz);
