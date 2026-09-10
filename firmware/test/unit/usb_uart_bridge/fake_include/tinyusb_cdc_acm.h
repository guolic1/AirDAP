#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    TINYUSB_CDC_ACM_0 = 0,
} tinyusb_cdcacm_itf_t;

typedef struct {
    uint32_t bit_rate;
    uint8_t stop_bits;
    uint8_t parity;
    uint8_t data_bits;
} cdc_line_coding_t;

typedef enum {
    CDC_EVENT_RX = 0,
    CDC_EVENT_LINE_STATE_CHANGED,
    CDC_EVENT_LINE_CODING_CHANGED,
} cdcacm_event_type_t;

typedef struct {
    cdcacm_event_type_t type;
    union {
        struct {
            bool dtr;
            bool rts;
        } line_state_changed_data;
        struct {
            const cdc_line_coding_t *p_line_coding;
        } line_coding_changed_data;
    };
} cdcacm_event_t;

typedef void (*tusb_cdcacm_callback_t)(int interface_number, cdcacm_event_t *event);

typedef struct {
    tinyusb_cdcacm_itf_t cdc_port;
    tusb_cdcacm_callback_t callback_rx;
    tusb_cdcacm_callback_t callback_rx_wanted_char;
    tusb_cdcacm_callback_t callback_line_state_changed;
    tusb_cdcacm_callback_t callback_line_coding_changed;
} tinyusb_config_cdcacm_t;

esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *config);
esp_err_t tinyusb_cdcacm_read(
    int interface_number,
    uint8_t *data,
    size_t capacity,
    size_t *received);
size_t tinyusb_cdcacm_write_queue(
    tinyusb_cdcacm_itf_t interface_number,
    const uint8_t *data,
    size_t length);
esp_err_t tinyusb_cdcacm_write_flush(
    tinyusb_cdcacm_itf_t interface_number,
    uint32_t timeout_ticks);
