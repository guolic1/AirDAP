#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t unused;
} gpio_dev_t;

extern gpio_dev_t fake_gpio_device;

#define GPIO_LL_GET_HW(instance) (&fake_gpio_device)

void gpio_ll_func_sel(gpio_dev_t *hw, uint8_t gpio_num, uint32_t function);
void gpio_ll_input_disable(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_input_enable(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_od_disable(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_od_enable(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_output_disable(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_output_enable(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_pulldown_dis(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_pullup_dis(gpio_dev_t *hw, uint32_t gpio_num);
void gpio_ll_set_level(gpio_dev_t *hw, uint32_t gpio_num, uint32_t level);
