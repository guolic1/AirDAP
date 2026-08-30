#pragma once

#include "esp_err.h"

void airdap_ota_initialize(void);
esp_err_t airdap_ota_confirm_running_image(void);
