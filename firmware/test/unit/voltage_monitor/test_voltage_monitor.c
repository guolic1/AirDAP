#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_board_pins.h"
#include "airdap_voltage_monitor.h"
#include "esp_adc/adc_cali_scheme.h"

enum {
    TARGET_RAW = 800,
    USB_RAW = 1000,
    EXPECTED_SAMPLES_PER_CHANNEL = 16,
};

static int unit_token;
static int target_calibration_token;
static int usb_calibration_token;
static unsigned int new_unit_calls;
static unsigned int channel_config_calls;
static unsigned int calibration_create_calls;
static unsigned int target_read_calls;
static unsigned int usb_read_calls;

esp_err_t adc_oneshot_io_to_channel(
    int io_num,
    adc_unit_t *unit_id,
    adc_channel_t *channel)
{
    assert(unit_id != NULL);
    assert(channel != NULL);
    *unit_id = ADC_UNIT_1;

    if (io_num == AIRDAP_PIN_TARGET_VTREF_ADC) {
        *channel = ADC_CHANNEL_2;
        return ESP_OK;
    }
    if (io_num == AIRDAP_PIN_USB_VBUS_SENSE) {
        *channel = ADC_CHANNEL_7;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t adc_oneshot_new_unit(
    const adc_oneshot_unit_init_cfg_t *init_config,
    adc_oneshot_unit_handle_t *ret_unit)
{
    assert(init_config != NULL);
    assert(ret_unit != NULL);
    assert(init_config->unit_id == ADC_UNIT_1);
    ++new_unit_calls;
    *ret_unit = &unit_token;
    return ESP_OK;
}

esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle)
{
    assert(handle == &unit_token);
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    const adc_oneshot_chan_cfg_t *config)
{
    assert(handle == &unit_token);
    assert(channel == ADC_CHANNEL_2 || channel == ADC_CHANNEL_7);
    assert(config != NULL);
    assert(config->atten == ADC_ATTEN_DB_12);
    assert(config->bitwidth == ADC_BITWIDTH_DEFAULT);
    ++channel_config_calls;
    return ESP_OK;
}

esp_err_t adc_cali_create_scheme_curve_fitting(
    const adc_cali_curve_fitting_config_t *config,
    adc_cali_handle_t *ret_handle)
{
    assert(config != NULL);
    assert(ret_handle != NULL);
    assert(config->unit_id == ADC_UNIT_1);
    assert(config->atten == ADC_ATTEN_DB_12);
    assert(config->bitwidth == ADC_BITWIDTH_DEFAULT);

    if (config->chan == ADC_CHANNEL_2) {
        *ret_handle = &target_calibration_token;
    } else {
        assert(config->chan == ADC_CHANNEL_7);
        *ret_handle = &usb_calibration_token;
    }
    ++calibration_create_calls;
    return ESP_OK;
}

esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle)
{
    assert(handle == &target_calibration_token || handle == &usb_calibration_token);
    return ESP_OK;
}

esp_err_t adc_oneshot_read(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    int *out_raw)
{
    assert(handle == &unit_token);
    assert(out_raw != NULL);

    if (channel == ADC_CHANNEL_2) {
        ++target_read_calls;
        *out_raw = TARGET_RAW;
    } else {
        assert(channel == ADC_CHANNEL_7);
        ++usb_read_calls;
        *out_raw = USB_RAW;
    }
    return ESP_OK;
}

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *voltage)
{
    assert(voltage != NULL);
    if (handle == &target_calibration_token) {
        assert(raw == TARGET_RAW);
    } else {
        assert(handle == &usb_calibration_token);
        assert(raw == USB_RAW);
    }
    *voltage = raw;
    return ESP_OK;
}

static void test_read_requires_initialization(void)
{
    airdap_voltage_reading_t reading;

    assert(airdap_voltage_monitor_read(&reading) == ESP_ERR_INVALID_STATE);
}

static void test_calibrated_divider_conversion(void)
{
    airdap_voltage_reading_t reading = {0};

    assert(airdap_voltage_monitor_init() == ESP_OK);
    assert(new_unit_calls == 1U);
    assert(channel_config_calls == 2U);
    assert(calibration_create_calls == 2U);

    /* Initialization is deliberately idempotent. */
    assert(airdap_voltage_monitor_init() == ESP_OK);
    assert(new_unit_calls == 1U);

    assert(airdap_voltage_monitor_read(&reading) == ESP_OK);
    assert(reading.target_mv == 1600U);
    assert(reading.usb_vbus_mv == 1682U);
    assert(target_read_calls == EXPECTED_SAMPLES_PER_CHANNEL);
    assert(usb_read_calls == EXPECTED_SAMPLES_PER_CHANNEL);
    assert(airdap_voltage_monitor_read(NULL) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_read_requires_initialization();
    test_calibrated_divider_conversion();

    puts("voltage monitor tests passed");
    return 0;
}
