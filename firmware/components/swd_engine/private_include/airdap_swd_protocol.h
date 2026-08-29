#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_swd.h"

typedef struct {
    void *context;
    uint8_t idle_cycles;
    bool data_phase;
    esp_err_t (*set_host_output)(void *context, bool enabled);
    esp_err_t (*write_bits)(void *context, uint64_t bits, size_t bit_count);
    esp_err_t (*read_bits)(void *context, size_t bit_count, uint64_t *bits);
    esp_err_t (*turnaround)(void *context, bool host_output);
} airdap_swd_io_t;

uint8_t airdap_swd_encode_request(const airdap_swd_request_t *request);

esp_err_t airdap_swd_protocol_transfer(
    const airdap_swd_io_t *io,
    const airdap_swd_request_t *request,
    uint32_t *data,
    unsigned int wait_retries,
    airdap_swd_ack_t *ack);

esp_err_t airdap_swd_protocol_connect(
    const airdap_swd_io_t *io,
    uint32_t *idcode);
