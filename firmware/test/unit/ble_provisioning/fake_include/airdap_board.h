#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t airdap_boot_key_get_pressed(bool *pressed);
esp_err_t airdap_board_leds_set(bool status_on, bool network_on);
