#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Keep 512-byte internal storage, but avoid a full-speed USB max-packet
     * multiple as the advertised CMSIS-DAP transfer limit. TinyUSB's vendor
     * stream appends a ZLP after an exact 512-byte response; a host that reads
     * exactly the advertised limit can otherwise receive that ZLP as the next
     * command's response. */
    AIRDAP_DAP_BUFFER_SIZE = 512,
    AIRDAP_DAP_PACKET_SIZE = 508,
    AIRDAP_DAP_PORT_DISABLED = 0,
    AIRDAP_DAP_PORT_SWD = 1,
};

_Static_assert(
    AIRDAP_DAP_PACKET_SIZE <= AIRDAP_DAP_BUFFER_SIZE,
    "CMSIS-DAP packet limit must fit the internal buffer");

typedef struct {
    void *context;
    bool (*connect)(void *context);
    void (*disconnect)(void *context);
    bool (*set_clock)(void *context, uint32_t clock_hz);
    bool (*configure_transfer)(
        void *context,
        uint8_t idle_cycles,
        uint16_t wait_retries);
    bool (*configure_swd)(
        void *context,
        uint8_t turnaround_cycles,
        bool data_phase);
    bool (*transfer)(
        void *context,
        bool ap,
        bool read,
        uint8_t address,
        uint32_t *data,
        uint8_t *status);
    bool (*write_sequence)(
        void *context,
        const uint8_t *data,
        size_t bit_count);
    bool (*read_sequence)(
        void *context,
        uint8_t *data,
        size_t bit_count);
    bool (*swj_pins)(
        void *context,
        uint8_t value,
        uint8_t select,
        uint32_t wait_us,
        uint8_t *pins);
    bool (*reset_target)(void *context);
    void (*delay_us)(void *context, uint16_t delay_us);
    void (*host_status)(
        void *context,
        uint8_t status_type,
        bool active);
    size_t (*vendor_command)(
        void *context,
        bool debug_connected,
        const uint8_t *request,
        size_t request_length,
        uint8_t *response,
        size_t response_capacity);
} airdap_dap_backend_t;

typedef struct {
    airdap_dap_backend_t backend;
    const char *serial_number;
    uint32_t match_mask;
    uint16_t match_retries;
    uint8_t selected_port;
} airdap_dap_processor_t;

void airdap_dap_processor_init(
    airdap_dap_processor_t *processor,
    const airdap_dap_backend_t *backend,
    const char *serial_number);

/* Returns 0 when more bytes are needed and SIZE_MAX for malformed input. */
size_t airdap_dap_request_size(
    const uint8_t *request,
    size_t available_length);

size_t airdap_dap_process_packet(
    airdap_dap_processor_t *processor,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity);

#ifdef __cplusplus
}
#endif
