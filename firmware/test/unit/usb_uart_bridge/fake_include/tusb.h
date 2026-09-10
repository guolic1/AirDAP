#pragma once

#include <stdbool.h>
#include <stdint.h>

bool tud_cdc_n_connected(uint8_t interface_number);
uint32_t tud_cdc_n_write_available(uint8_t interface_number);
