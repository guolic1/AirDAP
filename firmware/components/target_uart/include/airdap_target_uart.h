#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_TARGET_UART_DEFAULT_BAUD = 115200,
};

typedef enum {
    AIRDAP_TARGET_UART_TRANSPORT_USB = 0,
    AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
    AIRDAP_TARGET_UART_TRANSPORT_COUNT,
    AIRDAP_TARGET_UART_TRANSPORT_NONE = AIRDAP_TARGET_UART_TRANSPORT_COUNT,
} airdap_target_uart_transport_t;

typedef uint32_t airdap_target_uart_session_id_t;

typedef enum {
    AIRDAP_TARGET_UART_OK = 0,
    AIRDAP_TARGET_UART_ALREADY_OWNER,
    AIRDAP_TARGET_UART_INVALID_ARGUMENT,
    AIRDAP_TARGET_UART_INVALID_STATE,
    AIRDAP_TARGET_UART_UNAUTHENTICATED,
    AIRDAP_TARGET_UART_BUSY,
    AIRDAP_TARGET_UART_STALE_SESSION,
    AIRDAP_TARGET_UART_NOT_OWNER,
    AIRDAP_TARGET_UART_IO_ERROR,
} airdap_target_uart_result_t;

typedef struct {
    bool initialized;
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
    size_t rx_buffered_bytes;
    size_t tx_buffer_free_bytes;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t read_failures;
    uint32_t write_failures;
} airdap_target_uart_status_t;

typedef struct {
    size_t rx_buffered_bytes;
    uint32_t rx_dropped_bytes;
    bool tx_owner;
} airdap_target_uart_session_status_t;

esp_err_t airdap_target_uart_init(void);
esp_err_t airdap_target_uart_get_status(
    airdap_target_uart_status_t *status);

/* Each transport has at most one live session. Every live session receives an
 * independent, ordered copy of physical RX bytes. A full session buffer drops
 * only that session's newest bytes and reports them through rx_dropped_bytes;
 * closing the session discards its buffered bytes and counters.
 *
 * NETWORK callers may set authenticated only after validating the outer
 * transport session. The transport must revalidate immediately before each
 * privileged configure or write operation. */
airdap_target_uart_result_t airdap_target_uart_session_open(
    airdap_target_uart_transport_t transport,
    bool authenticated,
    airdap_target_uart_session_id_t *session);

airdap_target_uart_result_t airdap_target_uart_session_close(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session);

airdap_target_uart_result_t airdap_target_uart_session_get_status(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    airdap_target_uart_session_status_t *status);

/* TX ownership is first-come, non-preemptive, and retained until the exact
 * owner session closes. Only the owner may configure or write the UART. */
airdap_target_uart_result_t airdap_target_uart_tx_acquire(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session);

airdap_target_uart_result_t airdap_target_uart_session_configure(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    uint32_t baud_rate,
    uint8_t stop_bits,
    uint8_t parity,
    uint8_t data_bits);

airdap_target_uart_result_t airdap_target_uart_session_read(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    uint8_t *data,
    size_t capacity,
    size_t *received);

/* NETWORK writes accept only immediately enqueueable bytes; OK with written=0
 * means retry later. USB retains its existing buffered driver behavior. */
airdap_target_uart_result_t airdap_target_uart_session_write(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    const uint8_t *data,
    size_t length,
    size_t *written);

#ifdef __cplusplus
}
#endif
