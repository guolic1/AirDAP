#pragma once

#include "esp_err.h"

esp_err_t nvs_flash_init_partition(const char *partition_name);
esp_err_t nvs_flash_deinit_partition(const char *partition_name);
