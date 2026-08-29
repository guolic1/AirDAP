#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;
typedef int gpio_mode_t;
typedef int gpio_pullup_t;
typedef int gpio_pulldown_t;
typedef int gpio_int_type_t;

enum {
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
    GPIO_MODE_INPUT_OUTPUT_OD = 7,
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_INTR_DISABLE = 0,
};

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int gpio_get_level(gpio_num_t gpio_num);
