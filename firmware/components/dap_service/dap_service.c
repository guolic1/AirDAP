#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "airdap_dap.h"
#include "airdap_dap_protocol.h"
#include "airdap_dap_service.h"
#include "airdap_dap_service_internal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    DAP_QUEUE_DEPTH = 4,
    DAP_WORKER_STACK_SIZE = 4096,
    DAP_WORKER_PRIORITY = 6,
    DAP_WORKER_POLL_MS = 10,
};

typedef struct {
    airdap_dap_transport_t transport;
    airdap_dap_session_id_t session;
    airdap_dap_response_token_t response_token;
    airdap_dap_response_fn response_callback;
    void *response_context;
    int64_t queued_at_us;
    size_t request_length;
    uint8_t request[AIRDAP_DAP_BUFFER_SIZE];
} dap_work_item_t;

typedef struct {
    atomic_uint requests_accepted;
    atomic_uint requests_processed;
    atomic_uint responses_delivered;
    atomic_uint queue_full;
    atomic_uint timed_out;
    atomic_uint stale_requests;
    atomic_uint stale_responses;
    atomic_uint delivery_failures;
} dap_service_atomic_stats_t;

static QueueHandle_t dap_queue;
static atomic_bool initialization_started;
static atomic_bool initialized;
static atomic_uint next_session = 1U;
static atomic_uint live_sessions[AIRDAP_DAP_TRANSPORT_COUNT];
static atomic_uint pending_disconnects;
static SemaphoreHandle_t session_mutexes[AIRDAP_DAP_TRANSPORT_COUNT];
static dap_service_atomic_stats_t service_stats;

static bool valid_transport(airdap_dap_transport_t transport)
{
    return transport >= AIRDAP_DAP_TRANSPORT_USB &&
        transport < AIRDAP_DAP_TRANSPORT_COUNT;
}

static airdap_dap_owner_t transport_owner(
    airdap_dap_transport_t transport)
{
    switch (transport) {
    case AIRDAP_DAP_TRANSPORT_USB:
        return AIRDAP_DAP_OWNER_USB;
    case AIRDAP_DAP_TRANSPORT_NETWORK:
        return AIRDAP_DAP_OWNER_NETWORK;
    default:
        return AIRDAP_DAP_OWNER_NONE;
    }
}

static bool session_is_live(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session)
{
    return valid_transport(transport) && session != 0U &&
        atomic_load(&live_sessions[transport]) == session;
}

static bool session_lock(airdap_dap_transport_t transport)
{
    return xSemaphoreTake(
        session_mutexes[transport],
        portMAX_DELAY) == pdTRUE;
}

static void session_unlock(airdap_dap_transport_t transport)
{
    (void) xSemaphoreGive(session_mutexes[transport]);
}

static void request_disconnect(airdap_dap_transport_t transport)
{
    (void) atomic_fetch_or(
        &pending_disconnects,
        1U << (unsigned int) transport);
}

static bool process_pending_disconnects(void)
{
    const unsigned int pending = atomic_exchange(&pending_disconnects, 0U);
    if (pending == 0U) {
        return false;
    }

    for (unsigned int transport = AIRDAP_DAP_TRANSPORT_USB;
         transport < AIRDAP_DAP_TRANSPORT_COUNT;
         ++transport) {
        if ((pending & (1U << transport)) != 0U) {
            airdap_dap_session_closed(transport_owner(
                (airdap_dap_transport_t) transport));
        }
    }
    return true;
}

static bool request_timed_out(
    const dap_work_item_t *item,
    int64_t now_us)
{
    return now_us >= item->queued_at_us &&
        now_us - item->queued_at_us >=
            AIRDAP_DAP_SERVICE_REQUEST_TIMEOUT_US;
}

bool airdap_dap_service_process_next(void)
{
    bool did_work = process_pending_disconnects();
    dap_work_item_t item;
    if (xQueueReceive(
        dap_queue,
        &item,
        pdMS_TO_TICKS(DAP_WORKER_POLL_MS)) != pdTRUE) {
        return did_work;
    }
    did_work = true;

    /* A close may have raced the queue receive. Drain it before the item so a
     * replacement session never runs against the previous processor state. */
    (void) process_pending_disconnects();
    if (!session_is_live(item.transport, item.session)) {
        (void) atomic_fetch_add(&service_stats.stale_requests, 1U);
        return did_work;
    }
    if (request_timed_out(&item, esp_timer_get_time())) {
        (void) atomic_fetch_add(&service_stats.timed_out, 1U);
        return did_work;
    }

    uint8_t response[AIRDAP_DAP_BUFFER_SIZE];
    const size_t response_length = airdap_dap_process(
        transport_owner(item.transport),
        item.request,
        item.request_length,
        response,
        AIRDAP_DAP_PACKET_SIZE);
    (void) atomic_fetch_add(&service_stats.requests_processed, 1U);
    if (response_length == 0U) {
        return did_work;
    }
    if (response_length > AIRDAP_DAP_PACKET_SIZE) {
        (void) atomic_fetch_add(&service_stats.delivery_failures, 1U);
        return did_work;
    }
    if (request_timed_out(&item, esp_timer_get_time())) {
        (void) atomic_fetch_add(&service_stats.timed_out, 1U);
        return did_work;
    }
    if (!session_lock(item.transport)) {
        (void) atomic_fetch_add(&service_stats.delivery_failures, 1U);
        return did_work;
    }
    if (!session_is_live(item.transport, item.session)) {
        (void) atomic_fetch_add(&service_stats.stale_responses, 1U);
        session_unlock(item.transport);
        return did_work;
    }

    const bool delivered = item.response_callback(
        item.response_context,
        item.transport,
        item.session,
        item.response_token,
        response,
        response_length);
    session_unlock(item.transport);
    if (!delivered) {
        (void) atomic_fetch_add(&service_stats.delivery_failures, 1U);
        return did_work;
    }
    (void) atomic_fetch_add(&service_stats.responses_delivered, 1U);
    return did_work;
}

static void dap_worker_task(void *argument)
{
    (void) argument;
    for (;;) {
        (void) airdap_dap_service_process_next();
    }
}

esp_err_t airdap_dap_service_init(
    const char *serial_number,
    const char *firmware_version)
{
    if (serial_number == NULL || firmware_version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&initialization_started, true)) {
        return ESP_ERR_INVALID_STATE;
    }

    dap_queue = xQueueCreate(DAP_QUEUE_DEPTH, sizeof(dap_work_item_t));
    if (dap_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (unsigned int transport = AIRDAP_DAP_TRANSPORT_USB;
         transport < AIRDAP_DAP_TRANSPORT_COUNT;
         ++transport) {
        session_mutexes[transport] = xSemaphoreCreateMutex();
        if (session_mutexes[transport] == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const esp_err_t error = airdap_dap_init(
        serial_number,
        firmware_version);
    if (error != ESP_OK) {
        return error;
    }
    if (xTaskCreatePinnedToCore(
        dap_worker_task,
        "dap_worker",
        DAP_WORKER_STACK_SIZE,
        NULL,
        DAP_WORKER_PRIORITY,
        NULL,
        1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&initialized, true);
    return ESP_OK;
}

airdap_dap_service_result_t airdap_dap_service_session_open(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t *session)
{
    if (!valid_transport(transport) || session == NULL) {
        return AIRDAP_DAP_SERVICE_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }

    if (!session_lock(transport)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }
    if (atomic_load(&live_sessions[transport]) != 0U) {
        session_unlock(transport);
        return AIRDAP_DAP_SERVICE_BUSY;
    }
    airdap_dap_session_id_t new_session;
    do {
        new_session = atomic_fetch_add(&next_session, 1U);
    } while (new_session == 0U);
    atomic_store(&live_sessions[transport], new_session);
    session_unlock(transport);
    *session = new_session;
    return AIRDAP_DAP_SERVICE_OK;
}

airdap_dap_service_result_t airdap_dap_service_session_close(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session)
{
    if (!valid_transport(transport) || session == 0U) {
        return AIRDAP_DAP_SERVICE_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }

    if (!session_lock(transport)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }
    if (!session_is_live(transport, session)) {
        session_unlock(transport);
        return AIRDAP_DAP_SERVICE_STALE_SESSION;
    }
    atomic_store(&live_sessions[transport], 0U);
    request_disconnect(transport);
    session_unlock(transport);
    return AIRDAP_DAP_SERVICE_OK;
}

airdap_dap_service_result_t airdap_dap_service_session_reset(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session)
{
    if (!valid_transport(transport) || session == 0U) {
        return AIRDAP_DAP_SERVICE_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }
    if (!session_lock(transport)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }
    if (!session_is_live(transport, session)) {
        session_unlock(transport);
        return AIRDAP_DAP_SERVICE_STALE_SESSION;
    }
    request_disconnect(transport);
    session_unlock(transport);
    return AIRDAP_DAP_SERVICE_OK;
}

airdap_dap_service_result_t airdap_dap_service_submit(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session,
    const uint8_t *request,
    size_t request_length,
    airdap_dap_response_token_t response_token,
    airdap_dap_response_fn response_callback,
    void *response_context)
{
    if (!valid_transport(transport) || session == 0U || request == NULL ||
        request_length == 0U ||
        request_length > AIRDAP_DAP_PACKET_SIZE ||
        response_callback == NULL) {
        return AIRDAP_DAP_SERVICE_INVALID_ARGUMENT;
    }
    if (!atomic_load(&initialized)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }
    if (!session_lock(transport)) {
        return AIRDAP_DAP_SERVICE_INVALID_STATE;
    }
    if (!session_is_live(transport, session)) {
        session_unlock(transport);
        return AIRDAP_DAP_SERVICE_STALE_SESSION;
    }

    dap_work_item_t item = {
        .transport = transport,
        .session = session,
        .response_token = response_token,
        .response_callback = response_callback,
        .response_context = response_context,
        .queued_at_us = esp_timer_get_time(),
        .request_length = request_length,
    };
    memcpy(item.request, request, request_length);
    if (xQueueSend(dap_queue, &item, 0) != pdTRUE) {
        (void) atomic_fetch_add(&service_stats.queue_full, 1U);
        session_unlock(transport);
        return AIRDAP_DAP_SERVICE_QUEUE_FULL;
    }
    (void) atomic_fetch_add(&service_stats.requests_accepted, 1U);
    session_unlock(transport);
    return AIRDAP_DAP_SERVICE_OK;
}

void airdap_dap_service_get_stats(
    airdap_dap_service_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    stats->requests_accepted = atomic_load(
        &service_stats.requests_accepted);
    stats->requests_processed = atomic_load(
        &service_stats.requests_processed);
    stats->responses_delivered = atomic_load(
        &service_stats.responses_delivered);
    stats->queue_full = atomic_load(&service_stats.queue_full);
    stats->timed_out = atomic_load(&service_stats.timed_out);
    stats->stale_requests = atomic_load(&service_stats.stale_requests);
    stats->stale_responses = atomic_load(&service_stats.stale_responses);
    stats->delivery_failures = atomic_load(
        &service_stats.delivery_failures);
}
