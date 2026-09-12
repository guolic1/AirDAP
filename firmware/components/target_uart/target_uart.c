#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "airdap_board_pins.h"
#include "airdap_target_uart.h"
#include "airdap_target_uart_internal.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    TARGET_UART_PORT = UART_NUM_1,
    TARGET_UART_RX_BUFFER_SIZE = 2048,
    TARGET_UART_TX_BUFFER_SIZE = 2048,
    TARGET_UART_MAX_BAUD = 5000000,
    TARGET_UART_WORKER_STACK_SIZE = 3072,
    TARGET_UART_WORKER_PRIORITY = 5,
    TARGET_UART_IO_CHUNK = 256,
    TARGET_UART_READ_POLL_MS = 20,
};

typedef struct {
    airdap_target_uart_session_id_t session;
    size_t rx_head;
    size_t rx_count;
    uint32_t rx_dropped_bytes;
    uint8_t rx_buffer[AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE];
} target_uart_session_slot_t;

typedef struct {
    airdap_target_uart_transport_t transport;
    airdap_target_uart_session_id_t session;
} target_uart_tx_owner_t;

typedef struct {
    atomic_uint sequence;
    atomic_uint baud_rate;
    atomic_uint data_bits;
    atomic_uint parity;
    atomic_uint stop_bits;
    atomic_uint rx_bytes;
    atomic_uint tx_bytes;
    atomic_uint read_failures;
    atomic_uint write_failures;
} target_uart_diagnostic_state_t;

static atomic_bool initialization_started;
static atomic_bool initialized;
static atomic_uint next_session = 1U;
static SemaphoreHandle_t state_mutex;
static SemaphoreHandle_t tx_operation_mutex;
static target_uart_session_slot_t session_slots[
    AIRDAP_TARGET_UART_TRANSPORT_COUNT];
static target_uart_tx_owner_t tx_owner = {
    .transport = AIRDAP_TARGET_UART_TRANSPORT_NONE,
};
static target_uart_diagnostic_state_t diagnostic_state;

static bool valid_transport(airdap_target_uart_transport_t transport)
{
    return transport >= AIRDAP_TARGET_UART_TRANSPORT_USB &&
        transport < AIRDAP_TARGET_UART_TRANSPORT_COUNT;
}

static bool state_lock(void)
{
    return state_mutex != NULL &&
        xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE;
}

static void state_unlock(void)
{
    (void) xSemaphoreGive(state_mutex);
}

static bool tx_operation_lock(void)
{
    return tx_operation_mutex != NULL &&
        xSemaphoreTake(tx_operation_mutex, portMAX_DELAY) == pdTRUE;
}

static void tx_operation_unlock(void)
{
    (void) xSemaphoreGive(tx_operation_mutex);
}

static target_uart_session_slot_t *session_slot(
    airdap_target_uart_transport_t transport)
{
    return &session_slots[(unsigned int) transport];
}

static bool session_is_live_locked(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session)
{
    return session_slot(transport)->session == session;
}

static airdap_target_uart_result_t validate_live_session_locked(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session)
{
    return session_is_live_locked(transport, session)
        ? AIRDAP_TARGET_UART_OK
        : AIRDAP_TARGET_UART_STALE_SESSION;
}

static airdap_target_uart_result_t validate_tx_owner_locked(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session)
{
    const airdap_target_uart_result_t session_result =
        validate_live_session_locked(transport, session);
    if (session_result != AIRDAP_TARGET_UART_OK) {
        return session_result;
    }
    return tx_owner.transport == transport && tx_owner.session == session
        ? AIRDAP_TARGET_UART_OK
        : AIRDAP_TARGET_UART_NOT_OWNER;
}

static void publish_configuration(
    uint32_t baud_rate,
    uint8_t stop_bits,
    uint8_t parity,
    uint8_t data_bits)
{
    (void) atomic_fetch_add_explicit(
        &diagnostic_state.sequence,
        1U,
        memory_order_acq_rel);
    atomic_store_explicit(
        &diagnostic_state.baud_rate,
        baud_rate,
        memory_order_relaxed);
    atomic_store_explicit(
        &diagnostic_state.stop_bits,
        stop_bits,
        memory_order_relaxed);
    atomic_store_explicit(
        &diagnostic_state.parity,
        parity,
        memory_order_relaxed);
    atomic_store_explicit(
        &diagnostic_state.data_bits,
        data_bits,
        memory_order_relaxed);
    (void) atomic_fetch_add_explicit(
        &diagnostic_state.sequence,
        1U,
        memory_order_release);
}

static bool map_data_bits(uint8_t value, uart_word_length_t *data_bits)
{
    switch (value) {
    case 5:
        *data_bits = UART_DATA_5_BITS;
        return true;
    case 6:
        *data_bits = UART_DATA_6_BITS;
        return true;
    case 7:
        *data_bits = UART_DATA_7_BITS;
        return true;
    case 8:
        *data_bits = UART_DATA_8_BITS;
        return true;
    default:
        return false;
    }
}

static bool map_stop_bits(uint8_t value, uart_stop_bits_t *stop_bits)
{
    switch (value) {
    case 0:
        *stop_bits = UART_STOP_BITS_1;
        return true;
    case 1:
        *stop_bits = UART_STOP_BITS_1_5;
        return true;
    case 2:
        *stop_bits = UART_STOP_BITS_2;
        return true;
    default:
        return false;
    }
}

static bool map_parity(uint8_t value, uart_parity_t *parity)
{
    switch (value) {
    case 0:
        *parity = UART_PARITY_DISABLE;
        return true;
    case 1:
        *parity = UART_PARITY_ODD;
        return true;
    case 2:
        *parity = UART_PARITY_EVEN;
        return true;
    default:
        return false;
    }
}

static esp_err_t configure_driver(
    uint32_t baud_rate,
    uint8_t stop_bits,
    uint8_t parity,
    uint8_t data_bits)
{
    uart_word_length_t uart_data_bits;
    uart_stop_bits_t uart_stop_bits;
    uart_parity_t uart_parity;

    if (baud_rate == 0U || baud_rate > TARGET_UART_MAX_BAUD ||
        !map_data_bits(data_bits, &uart_data_bits) ||
        !map_stop_bits(stop_bits, &uart_stop_bits) ||
        !map_parity(parity, &uart_parity)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uart_config_t config = {
        .baud_rate = (int) baud_rate,
        .data_bits = uart_data_bits,
        .parity = uart_parity,
        .stop_bits = uart_stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    const esp_err_t error = uart_param_config(TARGET_UART_PORT, &config);
    if (error == ESP_OK) {
        publish_configuration(baud_rate, stop_bits, parity, data_bits);
    }
    return error;
}

static void add_dropped_bytes(
    target_uart_session_slot_t *slot,
    size_t dropped)
{
    if (dropped >= UINT32_MAX - slot->rx_dropped_bytes) {
        slot->rx_dropped_bytes = UINT32_MAX;
    } else {
        slot->rx_dropped_bytes += (uint32_t) dropped;
    }
}

static void reset_session_slot(target_uart_session_slot_t *slot)
{
    memset(slot, 0, sizeof(*slot));
}

void airdap_target_uart_publish_rx(const uint8_t *data, size_t length)
{
    if (!atomic_load(&initialized) || data == NULL || length == 0U ||
        !state_lock()) {
        return;
    }

    for (unsigned int index = AIRDAP_TARGET_UART_TRANSPORT_USB;
         index < AIRDAP_TARGET_UART_TRANSPORT_COUNT;
         ++index) {
        target_uart_session_slot_t *slot = &session_slots[index];
        if (slot->session == 0U) {
            continue;
        }
        const size_t available =
            AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE - slot->rx_count;
        const size_t accepted = length < available ? length : available;
        const size_t tail = (slot->rx_head + slot->rx_count) %
            AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE;
        const size_t first = accepted <
            AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE - tail
            ? accepted
            : AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE - tail;
        memcpy(slot->rx_buffer + tail, data, first);
        memcpy(slot->rx_buffer, data + first, accepted - first);
        slot->rx_count += accepted;
        add_dropped_bytes(slot, length - accepted);
    }
    state_unlock();
}

bool airdap_target_uart_process_rx_once(void)
{
    if (!atomic_load(&initialized)) {
        return false;
    }

    uint8_t data[TARGET_UART_IO_CHUNK];
    const int received = uart_read_bytes(
        TARGET_UART_PORT,
        data,
        sizeof(data),
        pdMS_TO_TICKS(TARGET_UART_READ_POLL_MS));
    if (received < 0) {
        (void) atomic_fetch_add(&diagnostic_state.read_failures, 1U);
        return false;
    }
    if (received == 0) {
        return false;
    }
    (void) atomic_fetch_add(
        &diagnostic_state.rx_bytes,
        (unsigned int) received);
    airdap_target_uart_publish_rx(data, (size_t) received);
    return true;
}

static void target_uart_worker(void *argument)
{
    (void) argument;
    for (;;) {
        (void) airdap_target_uart_process_rx_once();
    }
}

static void cleanup_failed_initialization(bool driver_installed)
{
    atomic_store(&initialized, false);
    if (driver_installed) {
        (void) uart_driver_delete(TARGET_UART_PORT);
    }
    if (tx_operation_mutex != NULL) {
        vSemaphoreDelete(tx_operation_mutex);
        tx_operation_mutex = NULL;
    }
    if (state_mutex != NULL) {
        vSemaphoreDelete(state_mutex);
        state_mutex = NULL;
    }
    atomic_store(&initialization_started, false);
}

esp_err_t airdap_target_uart_init(void)
{
    if (atomic_load(&initialized)) {
        return ESP_OK;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(
        &initialization_started,
        &expected,
        true)) {
        return ESP_ERR_INVALID_STATE;
    }

    state_mutex = xSemaphoreCreateMutex();
    tx_operation_mutex = xSemaphoreCreateMutex();
    if (state_mutex == NULL || tx_operation_mutex == NULL) {
        cleanup_failed_initialization(false);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = configure_driver(
        AIRDAP_TARGET_UART_DEFAULT_BAUD,
        0U,
        0U,
        8U);
    if (error != ESP_OK) {
        cleanup_failed_initialization(false);
        return error;
    }

    error = uart_set_pin(
        TARGET_UART_PORT,
        AIRDAP_PIN_TARGET_TX_TDI,
        AIRDAP_PIN_TARGET_RX_TDO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if (error != ESP_OK) {
        cleanup_failed_initialization(false);
        return error;
    }

    error = uart_driver_install(
        TARGET_UART_PORT,
        TARGET_UART_RX_BUFFER_SIZE,
        TARGET_UART_TX_BUFFER_SIZE,
        0,
        NULL,
        0);
    if (error != ESP_OK) {
        cleanup_failed_initialization(false);
        return error;
    }

    /* The new worker may preempt this task immediately. Publish readiness
     * before creation so its first iteration enters the blocking UART read. */
    atomic_store(&initialized, true);
    if (xTaskCreatePinnedToCore(
        target_uart_worker,
        "target_uart",
        TARGET_UART_WORKER_STACK_SIZE,
        NULL,
        TARGET_UART_WORKER_PRIORITY,
        NULL,
        1) != pdPASS) {
        cleanup_failed_initialization(true);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t airdap_target_uart_get_status(
    airdap_target_uart_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));

    unsigned int before;
    unsigned int after;
    do {
        before = atomic_load_explicit(
            &diagnostic_state.sequence,
            memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        status->baud_rate = atomic_load_explicit(
            &diagnostic_state.baud_rate,
            memory_order_relaxed);
        status->stop_bits = (uint8_t) atomic_load_explicit(
            &diagnostic_state.stop_bits,
            memory_order_relaxed);
        status->parity = (uint8_t) atomic_load_explicit(
            &diagnostic_state.parity,
            memory_order_relaxed);
        status->data_bits = (uint8_t) atomic_load_explicit(
            &diagnostic_state.data_bits,
            memory_order_relaxed);
        after = atomic_load_explicit(
            &diagnostic_state.sequence,
            memory_order_acquire);
    } while (before != after || (after & 1U) != 0U);

    status->initialized = atomic_load(&initialized);
    status->rx_bytes = atomic_load(&diagnostic_state.rx_bytes);
    status->tx_bytes = atomic_load(&diagnostic_state.tx_bytes);
    status->read_failures = atomic_load(&diagnostic_state.read_failures);
    status->write_failures = atomic_load(&diagnostic_state.write_failures);
    if (!status->initialized) {
        return ESP_OK;
    }

    esp_err_t error = uart_get_buffered_data_len(
        TARGET_UART_PORT,
        &status->rx_buffered_bytes);
    if (error == ESP_OK) {
        error = uart_get_tx_buffer_free_size(
            TARGET_UART_PORT,
            &status->tx_buffer_free_bytes);
    }
    return error;
}

airdap_target_uart_result_t airdap_target_uart_session_open(
    airdap_target_uart_transport_t transport,
    bool authenticated,
    airdap_target_uart_session_id_t *session)
{
    if (!valid_transport(transport) || session == NULL) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    *session = 0U;
    if (!atomic_load(&initialized)) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (transport == AIRDAP_TARGET_UART_TRANSPORT_NETWORK &&
        !authenticated) {
        return AIRDAP_TARGET_UART_UNAUTHENTICATED;
    }
    if (!state_lock()) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }

    target_uart_session_slot_t *slot = session_slot(transport);
    if (slot->session != 0U) {
        state_unlock();
        return AIRDAP_TARGET_UART_BUSY;
    }
    airdap_target_uart_session_id_t new_session;
    do {
        new_session = atomic_fetch_add(&next_session, 1U);
    } while (new_session == 0U);
    reset_session_slot(slot);
    slot->session = new_session;
    state_unlock();
    *session = new_session;
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_session_close(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session)
{
    if (!valid_transport(transport) || session == 0U) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!tx_operation_lock()) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!state_lock()) {
        tx_operation_unlock();
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!session_is_live_locked(transport, session)) {
        state_unlock();
        tx_operation_unlock();
        return AIRDAP_TARGET_UART_STALE_SESSION;
    }
    if (tx_owner.transport == transport && tx_owner.session == session) {
        tx_owner = (target_uart_tx_owner_t) {
            .transport = AIRDAP_TARGET_UART_TRANSPORT_NONE,
        };
    }
    reset_session_slot(session_slot(transport));
    state_unlock();
    tx_operation_unlock();
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_session_get_status(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    airdap_target_uart_session_status_t *status)
{
    if (!valid_transport(transport) || session == 0U || status == NULL) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    memset(status, 0, sizeof(*status));
    if (!atomic_load(&initialized)) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!state_lock()) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!session_is_live_locked(transport, session)) {
        state_unlock();
        return AIRDAP_TARGET_UART_STALE_SESSION;
    }
    const target_uart_session_slot_t *slot = session_slot(transport);
    status->rx_buffered_bytes = slot->rx_count;
    status->rx_dropped_bytes = slot->rx_dropped_bytes;
    status->tx_owner = tx_owner.transport == transport &&
        tx_owner.session == session;
    state_unlock();
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_tx_acquire(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session)
{
    if (!valid_transport(transport) || session == 0U) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!tx_operation_lock()) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!state_lock()) {
        tx_operation_unlock();
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }

    const airdap_target_uart_result_t session_result =
        validate_live_session_locked(transport, session);
    airdap_target_uart_result_t result = session_result;
    if (session_result == AIRDAP_TARGET_UART_OK) {
        if (tx_owner.transport == AIRDAP_TARGET_UART_TRANSPORT_NONE) {
            tx_owner.transport = transport;
            tx_owner.session = session;
            result = AIRDAP_TARGET_UART_OK;
        } else if (tx_owner.transport == transport &&
                   tx_owner.session == session) {
            result = AIRDAP_TARGET_UART_ALREADY_OWNER;
        } else {
            result = AIRDAP_TARGET_UART_BUSY;
        }
    }
    state_unlock();
    tx_operation_unlock();
    return result;
}

airdap_target_uart_result_t airdap_target_uart_session_configure(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    uint32_t baud_rate,
    uint8_t stop_bits,
    uint8_t parity,
    uint8_t data_bits)
{
    if (!valid_transport(transport) || session == 0U) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!tx_operation_lock()) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!state_lock()) {
        tx_operation_unlock();
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    const airdap_target_uart_result_t owner_result =
        validate_tx_owner_locked(transport, session);
    state_unlock();
    if (owner_result != AIRDAP_TARGET_UART_OK) {
        tx_operation_unlock();
        return owner_result;
    }

    const esp_err_t error = configure_driver(
        baud_rate,
        stop_bits,
        parity,
        data_bits);
    tx_operation_unlock();
    if (error == ESP_ERR_INVALID_ARG) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    return error == ESP_OK
        ? AIRDAP_TARGET_UART_OK
        : AIRDAP_TARGET_UART_IO_ERROR;
}

airdap_target_uart_result_t airdap_target_uart_session_read(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    uint8_t *data,
    size_t capacity,
    size_t *received)
{
    if (received != NULL) {
        *received = 0U;
    }
    if (!valid_transport(transport) || session == 0U || data == NULL ||
        capacity == 0U || received == NULL) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!state_lock()) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!session_is_live_locked(transport, session)) {
        state_unlock();
        return AIRDAP_TARGET_UART_STALE_SESSION;
    }

    target_uart_session_slot_t *slot = session_slot(transport);
    const size_t count = capacity < slot->rx_count
        ? capacity
        : slot->rx_count;
    const size_t first = count <
        AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE - slot->rx_head
        ? count
        : AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE - slot->rx_head;
    memcpy(data, slot->rx_buffer + slot->rx_head, first);
    memcpy(data + first, slot->rx_buffer, count - first);
    slot->rx_head = (slot->rx_head + count) %
        AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE;
    slot->rx_count -= count;
    state_unlock();
    *received = count;
    return AIRDAP_TARGET_UART_OK;
}

airdap_target_uart_result_t airdap_target_uart_session_write(
    airdap_target_uart_transport_t transport,
    airdap_target_uart_session_id_t session,
    const uint8_t *data,
    size_t length,
    size_t *written)
{
    if (written != NULL) {
        *written = 0U;
    }
    if (!valid_transport(transport) || session == 0U || data == NULL ||
        length == 0U || written == NULL) {
        return AIRDAP_TARGET_UART_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!tx_operation_lock()) {
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    if (!state_lock()) {
        tx_operation_unlock();
        return AIRDAP_TARGET_UART_INVALID_STATE;
    }
    const airdap_target_uart_result_t owner_result =
        validate_tx_owner_locked(transport, session);
    state_unlock();
    if (owner_result != AIRDAP_TARGET_UART_OK) {
        tx_operation_unlock();
        return owner_result;
    }

    if (transport == AIRDAP_TARGET_UART_TRANSPORT_NETWORK) {
        /* The TCP caller can retry a partial acceptance. Never hold the owner
         * lock waiting for ring space: even a very low baud must permit auth
         * revocation and disconnect cleanup. IDF's free-size API accounts for
         * the transaction descriptor; the sole writer holds this mutex. */
        size_t available = 0U;
        if (uart_get_tx_buffer_free_size(TARGET_UART_PORT, &available) != ESP_OK) {
            (void) atomic_fetch_add(&diagnostic_state.write_failures, 1U);
            tx_operation_unlock();
            return AIRDAP_TARGET_UART_IO_ERROR;
        }
        if (length > available) length = available;
        if (length == 0U) {
            tx_operation_unlock();
            return AIRDAP_TARGET_UART_OK;
        }
    }
    const int result = uart_write_bytes(TARGET_UART_PORT, data, length);
    tx_operation_unlock();
    if (result < 0) {
        (void) atomic_fetch_add(&diagnostic_state.write_failures, 1U);
        return AIRDAP_TARGET_UART_IO_ERROR;
    }
    *written = (size_t) result;
    if (result > 0) {
        (void) atomic_fetch_add(
            &diagnostic_state.tx_bytes,
            (unsigned int) result);
    }
    return AIRDAP_TARGET_UART_OK;
}
