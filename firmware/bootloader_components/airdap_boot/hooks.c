#include <stdint.h>

#include "sdkconfig.h"

#include "airdap_board_pins.h"
#include "hal/gpio_ll.h"
#include "soc/io_mux_reg.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "AirDAP bootloader hooks require an ESP32-S3 target"
#endif

static void disable_internal_pulls(gpio_dev_t *gpio, uint32_t pin)
{
    gpio_ll_pullup_dis(gpio, pin);
    gpio_ll_pulldown_dis(gpio, pin);
}

static void configure_push_pull_output(gpio_dev_t *gpio, uint32_t pin, uint32_t level)
{
    gpio_ll_output_disable(gpio, pin);
    gpio_ll_set_level(gpio, pin, level);
    gpio_ll_func_sel(gpio, pin, PIN_FUNC_GPIO);
    gpio_ll_od_disable(gpio, pin);
    disable_internal_pulls(gpio, pin);
    gpio_ll_input_disable(gpio, pin);
    gpio_ll_output_enable(gpio, pin);
}

static void configure_high_impedance_input(gpio_dev_t *gpio, uint32_t pin)
{
    gpio_ll_output_disable(gpio, pin);
    gpio_ll_func_sel(gpio, pin, PIN_FUNC_GPIO);
    gpio_ll_od_disable(gpio, pin);
    disable_internal_pulls(gpio, pin);
    gpio_ll_input_enable(gpio, pin);
}

static void configure_released_open_drain(gpio_dev_t *gpio, uint32_t pin)
{
    gpio_ll_output_disable(gpio, pin);
    gpio_ll_set_level(gpio, pin, 1U);
    gpio_ll_func_sel(gpio, pin, PIN_FUNC_GPIO);
    gpio_ll_od_enable(gpio, pin);
    disable_internal_pulls(gpio, pin);
    gpio_ll_input_enable(gpio, pin);
    gpio_ll_output_enable(gpio, pin);
}

/*
 * ESP-IDF declares the hooks as weak symbols in a static library. This symbol
 * forces the linker to include this component, as required by the bootloader
 * hook contract.
 */
void bootloader_hooks_include(void)
{
}

void bootloader_before_init(void)
{
    gpio_dev_t *gpio = GPIO_LL_GET_HW(0);

    /* Do not drive the target while the application image is being checked. */
    configure_push_pull_output(gpio, AIRDAP_PIN_TARGET_SWCLK_TCK, 0U);
    configure_high_impedance_input(gpio, AIRDAP_PIN_TARGET_SWDIO_TMS);
    configure_push_pull_output(gpio, AIRDAP_PIN_SWDIO_DIR, 0U);
    configure_push_pull_output(gpio, AIRDAP_PIN_TARGET_NRESET, 0U);

    /* GPIO9 may only pull the shared power-control/status net low. */
    configure_released_open_drain(gpio, AIRDAP_PIN_V_SOURCE_STATUS);

    configure_push_pull_output(gpio, AIRDAP_PIN_TARGET_TX_TDI, 1U);
    configure_high_impedance_input(gpio, AIRDAP_PIN_TARGET_RX_TDO);
}
