#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_SWD_DEFAULT_CLOCK_HZ = 5000000,
};

typedef enum {
    AIRDAP_SWD_PORT_DP = 0,
    AIRDAP_SWD_PORT_AP = 1,
} airdap_swd_port_t;

typedef enum {
    AIRDAP_SWD_WRITE = 0,
    AIRDAP_SWD_READ = 1,
} airdap_swd_direction_t;

typedef enum {
    AIRDAP_SWD_ACK_NONE = 0,
    AIRDAP_SWD_ACK_OK = 1,
    AIRDAP_SWD_ACK_WAIT = 2,
    AIRDAP_SWD_ACK_FAULT = 4,
} airdap_swd_ack_t;

typedef struct {
    airdap_swd_port_t port;
    airdap_swd_direction_t direction;
    uint8_t address;
} airdap_swd_request_t;

esp_err_t airdap_swd_init(uint32_t clock_hz);
esp_err_t airdap_swd_set_clock(uint32_t clock_hz);
void airdap_swd_set_wait_retries(unsigned int wait_retries);
esp_err_t airdap_swd_configure_transfer(
    uint8_t idle_cycles,
    unsigned int wait_retries);
esp_err_t airdap_swd_configure_bus(
    uint8_t turnaround_cycles,
    bool data_phase);

esp_err_t airdap_swd_write_sequence(
    const uint8_t *data,
    size_t bit_count);
esp_err_t airdap_swd_read_sequence(
    uint8_t *data,
    size_t bit_count);
esp_err_t airdap_swd_set_io_state(bool host_output);
esp_err_t airdap_swd_drive_pins(
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins);

esp_err_t airdap_swd_connect(uint32_t *idcode);

esp_err_t airdap_swd_transfer(
    const airdap_swd_request_t *request,
    uint32_t *data,
    airdap_swd_ack_t *ack);

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

esp_err_t airdap_swd_clear_errors(airdap_swd_ack_t *ack);

#ifdef __cplusplus
}
#endif
