#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_board_pins.h"
#include "airdap_voltage_monitor.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#if !ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
#error "AirDAP voltage monitoring requires ADC curve-fitting calibration"
#endif

enum {
    AIRDAP_ADC_SAMPLE_COUNT = 16,
    AIRDAP_TARGET_DIVIDER_MULTIPLIER = 2,
    AIRDAP_USB_DIVIDER_NUMERATOR = 370,
    AIRDAP_USB_DIVIDER_DENOMINATOR = 220,
};

typedef struct {
    adc_channel_t channel;
    adc_cali_handle_t calibration;
} monitored_channel_t;

static adc_oneshot_unit_handle_t adc_handle;
static monitored_channel_t target_channel;
static monitored_channel_t usb_channel;
static bool initialized;

static esp_err_t create_calibration(
    adc_unit_t unit,
    adc_channel_t channel,
    adc_cali_handle_t *calibration)
{
    const adc_cali_curve_fitting_config_t config = {
        .unit_id = unit,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    return adc_cali_create_scheme_curve_fitting(&config, calibration);
}

static esp_err_t read_calibrated_mv(
    const monitored_channel_t *channel,
    uint32_t *millivolts)
{
    uint32_t raw_sum = 0U;

    for (unsigned int sample = 0; sample < AIRDAP_ADC_SAMPLE_COUNT; ++sample) {
        int raw = 0;
        esp_err_t error = adc_oneshot_read(adc_handle, channel->channel, &raw);
        if (error != ESP_OK) {
            return error;
        }
        if (raw < 0) {
            return ESP_ERR_INVALID_STATE;
        }
        raw_sum += (uint32_t) raw;
    }

    const int averaged_raw =
        (int) ((raw_sum + (AIRDAP_ADC_SAMPLE_COUNT / 2U)) / AIRDAP_ADC_SAMPLE_COUNT);
    int calibrated_mv = 0;
    esp_err_t error = adc_cali_raw_to_voltage(
        channel->calibration,
        averaged_raw,
        &calibrated_mv);

    if (error != ESP_OK) {
        return error;
    }
    if (calibrated_mv < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *millivolts = (uint32_t) calibrated_mv;
    return ESP_OK;
}

esp_err_t airdap_voltage_monitor_init(void)
{
    adc_unit_t target_unit;
    adc_unit_t usb_unit;
    adc_channel_t target_adc_channel;
    adc_channel_t usb_adc_channel;
    adc_oneshot_unit_handle_t new_adc_handle = NULL;
    adc_cali_handle_t new_target_calibration = NULL;
    adc_cali_handle_t new_usb_calibration = NULL;
    esp_err_t error;

    if (initialized) {
        return ESP_OK;
    }

    error = adc_oneshot_io_to_channel(
        AIRDAP_PIN_TARGET_VTREF_ADC,
        &target_unit,
        &target_adc_channel);
    if (error != ESP_OK) {
        return error;
    }

    error = adc_oneshot_io_to_channel(
        AIRDAP_PIN_USB_VBUS_SENSE,
        &usb_unit,
        &usb_adc_channel);
    if (error != ESP_OK) {
        return error;
    }
    if (target_unit != usb_unit) {
        return ESP_ERR_INVALID_STATE;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = target_unit,
    };
    error = adc_oneshot_new_unit(&unit_config, &new_adc_handle);
    if (error != ESP_OK) {
        return error;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    error = adc_oneshot_config_channel(
        new_adc_handle,
        target_adc_channel,
        &channel_config);
    if (error != ESP_OK) {
        goto fail_unit;
    }

    error = adc_oneshot_config_channel(
        new_adc_handle,
        usb_adc_channel,
        &channel_config);
    if (error != ESP_OK) {
        goto fail_unit;
    }

    error = create_calibration(
        target_unit,
        target_adc_channel,
        &new_target_calibration);
    if (error != ESP_OK) {
        goto fail_unit;
    }

    error = create_calibration(
        usb_unit,
        usb_adc_channel,
        &new_usb_calibration);
    if (error != ESP_OK) {
        goto fail_target_calibration;
    }

    adc_handle = new_adc_handle;
    target_channel = (monitored_channel_t) {
        .channel = target_adc_channel,
        .calibration = new_target_calibration,
    };
    usb_channel = (monitored_channel_t) {
        .channel = usb_adc_channel,
        .calibration = new_usb_calibration,
    };
    initialized = true;
    return ESP_OK;

fail_target_calibration:
    (void) adc_cali_delete_scheme_curve_fitting(new_target_calibration);
fail_unit:
    (void) adc_oneshot_del_unit(new_adc_handle);
    return error;
}

esp_err_t airdap_voltage_monitor_read(airdap_voltage_reading_t *reading)
{
    uint32_t target_adc_mv;
    uint32_t usb_adc_mv;
    esp_err_t error;

    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    error = read_calibrated_mv(&target_channel, &target_adc_mv);
    if (error != ESP_OK) {
        return error;
    }
    error = read_calibrated_mv(&usb_channel, &usb_adc_mv);
    if (error != ESP_OK) {
        return error;
    }

    const airdap_voltage_reading_t new_reading = {
        .target_mv = target_adc_mv * AIRDAP_TARGET_DIVIDER_MULTIPLIER,
        .usb_vbus_mv = (uint32_t) (
            ((uint64_t) usb_adc_mv * AIRDAP_USB_DIVIDER_NUMERATOR +
             (AIRDAP_USB_DIVIDER_DENOMINATOR / 2U)) /
            AIRDAP_USB_DIVIDER_DENOMINATOR),
    };
    *reading = new_reading;
    return ESP_OK;
}
