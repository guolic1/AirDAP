#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AIRDAP_SWD_ACK_NONE = 0,
    AIRDAP_SWD_ACK_OK = 1,
} airdap_swd_ack_t;

esp_err_t airdap_swd_set_clock(uint32_t clock_hz);
esp_err_t airdap_swd_configure_transfer(
    uint8_t idle_cycles,
    unsigned int wait_retries);
esp_err_t airdap_swd_configure_bus(
    uint8_t turnaround_cycles,
    bool data_phase);
esp_err_t airdap_swd_set_io_state(bool host_output);
esp_err_t airdap_swd_write_sequence(
    const uint8_t *data,
    size_t bit_count);
esp_err_t airdap_swd_read_sequence(uint8_t *data, size_t bit_count);
esp_err_t airdap_swd_drive_pins(
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins);
esp_err_t airdap_swd_read_dp(
    uint8_t address,
    uint32_t *data,
    airdap_swd_ack_t *ack);
esp_err_t airdap_swd_write_dp(
    uint8_t address,
    uint32_t data,
    airdap_swd_ack_t *ack);
esp_err_t airdap_swd_read_ap(
    uint8_t address,
    uint32_t *data,
    airdap_swd_ack_t *ack);
esp_err_t airdap_swd_write_ap(
    uint8_t address,
    uint32_t data,
    airdap_swd_ack_t *ack);
