#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t airdap_boot_key_set_simulated_pressed(bool pressed);
esp_err_t airdap_boot_key_get_simulated_pressed(bool *pressed);
