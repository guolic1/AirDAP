#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap.h"
#include "airdap_dap_protocol.h"
#include "airdap_dap_service.h"
#include "airdap_dap_service_internal.h"
#include "esp_err.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    FAKE_QUEUE_CAPACITY = 4,
    FAKE_ITEM_CAPACITY = 1024,
    CAPTURE_CAPACITY = 16,
    FAKE_SEMAPHORE_CAPACITY = AIRDAP_DAP_TRANSPORT_COUNT,
};

typedef struct {
    size_t item_size;
    size_t head;
    size_t count;
    uint8_t items[FAKE_QUEUE_CAPACITY][FAKE_ITEM_CAPACITY];
} fake_queue_t;

typedef struct {
    airdap_dap_transport_t transports[CAPTURE_CAPACITY];
    airdap_dap_session_id_t sessions[CAPTURE_CAPACITY];
    airdap_dap_response_token_t tokens[CAPTURE_CAPACITY];
    uint8_t responses[CAPTURE_CAPACITY][4];
    size_t response_lengths[CAPTURE_CAPACITY];
    size_t count;
    bool accept;
} response_capture_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t close_thread;
    airdap_dap_transport_t transport;
    airdap_dap_session_id_t session;
    airdap_dap_service_result_t result;
    bool close_started;
    bool take_attempted;
    bool close_returned;
} close_barrier_t;

static fake_queue_t fake_queue;
static TaskFunction_t worker_task;
static int64_t fake_time_us;
static airdap_dap_owner_t processed_owners[CAPTURE_CAPACITY];
static size_t processed_count;
static airdap_dap_owner_t closed_owners[CAPTURE_CAPACITY];
static size_t closed_count;
static bool close_during_process;
static airdap_dap_transport_t transport_to_close;
static airdap_dap_session_id_t session_to_close;
static unsigned dap_init_calls;
static fake_semaphore_t fake_semaphores[FAKE_SEMAPHORE_CAPACITY];
static size_t fake_semaphore_count;
static pthread_mutex_t observed_close_mutex = PTHREAD_MUTEX_INITIALIZER;
static close_barrier_t *observed_close;
static _Thread_local bool observe_close_take;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    assert(fake_semaphore_count < FAKE_SEMAPHORE_CAPACITY);
    fake_semaphore_t *semaphore = &fake_semaphores[fake_semaphore_count++];
    assert(pthread_mutex_init(&semaphore->mutex, NULL) == 0);
    return semaphore;
}

BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t timeout_ticks)
{
    assert(semaphore != NULL && timeout_ticks == portMAX_DELAY);
    assert(pthread_mutex_lock(&observed_close_mutex) == 0);
    close_barrier_t *barrier = observed_close;
    if (barrier != NULL && observe_close_take) {
        assert(pthread_mutex_lock(&barrier->mutex) == 0);
        barrier->take_attempted = true;
        assert(pthread_cond_broadcast(&barrier->condition) == 0);
        assert(pthread_mutex_unlock(&barrier->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&observed_close_mutex) == 0);
    return pthread_mutex_lock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL);
    return pthread_mutex_unlock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

QueueHandle_t xQueueCreate(
    UBaseType_t queue_length,
    UBaseType_t item_size)
{
    assert(queue_length == FAKE_QUEUE_CAPACITY);
    assert(item_size <= FAKE_ITEM_CAPACITY);
    memset(&fake_queue, 0, sizeof(fake_queue));
    fake_queue.item_size = item_size;
    return &fake_queue;
}

BaseType_t xQueueSend(
    QueueHandle_t queue,
    const void *item,
    TickType_t ticks_to_wait)
{
    (void) ticks_to_wait;
    fake_queue_t *target = queue;
    assert(target == &fake_queue && item != NULL);
    if (target->count == FAKE_QUEUE_CAPACITY) {
        return pdFALSE;
    }
    const size_t tail = (target->head + target->count) % FAKE_QUEUE_CAPACITY;
    memcpy(target->items[tail], item, target->item_size);
    ++target->count;
    return pdTRUE;
}

BaseType_t xQueueReceive(
    QueueHandle_t queue,
    void *item,
    TickType_t ticks_to_wait)
{
    (void) ticks_to_wait;
    fake_queue_t *source = queue;
    assert(source == &fake_queue && item != NULL);
    if (source->count == 0U) {
        return pdFALSE;
    }
    memcpy(item, source->items[source->head], source->item_size);
    source->head = (source->head + 1U) % FAKE_QUEUE_CAPACITY;
    --source->count;
    return pdTRUE;
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core_id)
{
    assert(task != NULL && name != NULL && stack_depth > 0U);
    assert(argument == NULL && priority > 0U && core_id == 1);
    worker_task = task;
    if (handle != NULL) {
        *handle = &fake_queue;
    }
    return pdPASS;
}

int64_t esp_timer_get_time(void)
{
    return fake_time_us;
}

esp_err_t airdap_dap_init(
    const char *serial_number,
    const char *firmware_version)
{
    assert(serial_number != NULL && firmware_version != NULL);
    ++dap_init_calls;
    return ESP_OK;
}

size_t airdap_dap_process(
    airdap_dap_owner_t owner,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    assert(owner == AIRDAP_DAP_OWNER_USB ||
        owner == AIRDAP_DAP_OWNER_NETWORK);
    assert(request != NULL && request_length > 0U);
    assert(response != NULL && response_capacity == AIRDAP_DAP_PACKET_SIZE);
    assert(processed_count < CAPTURE_CAPACITY);
    processed_owners[processed_count++] = owner;
    response[0] = request[0];
    response[1] = (uint8_t) owner;

    if (close_during_process) {
        close_during_process = false;
        assert(airdap_dap_service_session_close(
            transport_to_close,
            session_to_close) == AIRDAP_DAP_SERVICE_OK);
    }
    return 2U;
}

void airdap_dap_session_closed(airdap_dap_owner_t owner)
{
    assert(closed_count < CAPTURE_CAPACITY);
    closed_owners[closed_count++] = owner;
}

static bool capture_response(
    void *context,
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session,
    airdap_dap_response_token_t token,
    const uint8_t *response,
    size_t response_length)
{
    response_capture_t *capture = context;
    assert(capture != NULL && response != NULL);
    assert(capture->count < CAPTURE_CAPACITY);
    assert(response_length <= sizeof(capture->responses[0]));
    const size_t index = capture->count++;
    capture->transports[index] = transport;
    capture->sessions[index] = session;
    capture->tokens[index] = token;
    capture->response_lengths[index] = response_length;
    memcpy(capture->responses[index], response, response_length);
    return capture->accept;
}

static void *close_session_thread(void *context)
{
    close_barrier_t *barrier = context;
    observe_close_take = true;
    assert(pthread_mutex_lock(&observed_close_mutex) == 0);
    observed_close = barrier;
    assert(pthread_mutex_unlock(&observed_close_mutex) == 0);

    assert(pthread_mutex_lock(&barrier->mutex) == 0);
    barrier->close_started = true;
    assert(pthread_cond_broadcast(&barrier->condition) == 0);
    assert(pthread_mutex_unlock(&barrier->mutex) == 0);

    const airdap_dap_service_result_t result =
        airdap_dap_service_session_close(
            barrier->transport,
            barrier->session);
    assert(pthread_mutex_lock(&barrier->mutex) == 0);
    barrier->result = result;
    barrier->close_returned = true;
    assert(pthread_cond_broadcast(&barrier->condition) == 0);
    assert(pthread_mutex_unlock(&barrier->mutex) == 0);
    return NULL;
}

static bool close_during_callback(
    void *context,
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session,
    airdap_dap_response_token_t token,
    const uint8_t *response,
    size_t response_length)
{
    (void) token;
    assert(response != NULL && response_length > 0U);
    close_barrier_t *barrier = context;
    assert(barrier != NULL && transport == barrier->transport &&
        session == barrier->session);
    assert(pthread_create(
        &barrier->close_thread,
        NULL,
        close_session_thread,
        barrier) == 0);

    assert(pthread_mutex_lock(&barrier->mutex) == 0);
    while (!barrier->close_started) {
        assert(pthread_cond_wait(
            &barrier->condition,
            &barrier->mutex) == 0);
    }
    while (!barrier->take_attempted && !barrier->close_returned) {
        assert(pthread_cond_wait(
            &barrier->condition,
            &barrier->mutex) == 0);
    }
    assert(barrier->take_attempted);
    assert(!barrier->close_returned);
    assert(pthread_mutex_unlock(&barrier->mutex) == 0);
    return true;
}

static void process_one(void)
{
    assert(airdap_dap_service_process_next());
}

static void test_interleaved_transport_routing(
    response_capture_t *usb,
    response_capture_t *network,
    airdap_dap_session_id_t *usb_session,
    airdap_dap_session_id_t *network_session)
{
    const uint8_t usb_request[] = {0x10U};
    const uint8_t network_request[] = {0x20U};

    assert(airdap_dap_service_session_open(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session) == AIRDAP_DAP_SERVICE_OK);
    assert(airdap_dap_service_session_open(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        network_session) == AIRDAP_DAP_SERVICE_OK);
    assert(*usb_session != 0U && *network_session != 0U);
    assert(airdap_dap_service_session_open(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session) == AIRDAP_DAP_SERVICE_BUSY);

    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        *usb_session,
        usb_request,
        sizeof(usb_request),
        0x1111U,
        capture_response,
        usb) == AIRDAP_DAP_SERVICE_OK);
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        *network_session,
        network_request,
        sizeof(network_request),
        0x2222U,
        capture_response,
        network) == AIRDAP_DAP_SERVICE_OK);

    process_one();
    process_one();
    assert(processed_count == 2U);
    assert(processed_owners[0] == AIRDAP_DAP_OWNER_USB);
    assert(processed_owners[1] == AIRDAP_DAP_OWNER_NETWORK);
    assert(usb->count == 1U && network->count == 1U);
    assert(usb->transports[0] == AIRDAP_DAP_TRANSPORT_USB);
    assert(usb->sessions[0] == *usb_session && usb->tokens[0] == 0x1111U);
    assert(usb->responses[0][0] == 0x10U &&
        usb->responses[0][1] == AIRDAP_DAP_OWNER_USB);
    assert(network->transports[0] == AIRDAP_DAP_TRANSPORT_NETWORK);
    assert(network->sessions[0] == *network_session &&
        network->tokens[0] == 0x2222U);
}

static void test_closed_inflight_response_is_not_delivered(
    response_capture_t *usb,
    airdap_dap_session_id_t *usb_session)
{
    const uint8_t old_request[] = {0x30U};
    const size_t responses_before = usb->count;
    const airdap_dap_session_id_t old_session = *usb_session;

    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        old_session,
        old_request,
        sizeof(old_request),
        0x3333U,
        capture_response,
        usb) == AIRDAP_DAP_SERVICE_OK);
    close_during_process = true;
    transport_to_close = AIRDAP_DAP_TRANSPORT_USB;
    session_to_close = old_session;
    process_one();
    assert(usb->count == responses_before);

    assert(airdap_dap_service_session_open(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session) == AIRDAP_DAP_SERVICE_OK);
    assert(*usb_session != old_session);
    const uint8_t new_request[] = {0x31U};
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        *usb_session,
        new_request,
        sizeof(new_request),
        0x3434U,
        capture_response,
        usb) == AIRDAP_DAP_SERVICE_OK);
    process_one();
    assert(closed_count == 1U && closed_owners[0] == AIRDAP_DAP_OWNER_USB);
    assert(usb->count == responses_before + 1U);
    assert(usb->sessions[responses_before] == *usb_session);
    assert(usb->tokens[responses_before] == 0x3434U);

    airdap_dap_service_stats_t stats;
    airdap_dap_service_get_stats(&stats);
    assert(stats.stale_responses == 1U);
}

static void test_closed_queued_request_is_discarded(
    response_capture_t *network,
    airdap_dap_session_id_t *network_session)
{
    const uint8_t old_request[] = {0x40U};
    const airdap_dap_session_id_t old_session = *network_session;
    const size_t responses_before = network->count;
    const size_t processed_before = processed_count;

    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        old_session,
        old_request,
        sizeof(old_request),
        0x4444U,
        capture_response,
        network) == AIRDAP_DAP_SERVICE_OK);
    assert(airdap_dap_service_session_close(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        old_session) == AIRDAP_DAP_SERVICE_OK);
    assert(airdap_dap_service_session_open(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        network_session) == AIRDAP_DAP_SERVICE_OK);
    const uint8_t new_request[] = {0x41U};
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        *network_session,
        new_request,
        sizeof(new_request),
        0x4545U,
        capture_response,
        network) == AIRDAP_DAP_SERVICE_OK);

    process_one();
    assert(processed_count == processed_before);
    assert(network->count == responses_before);
    process_one();
    assert(processed_count == processed_before + 1U);
    assert(network->count == responses_before + 1U);
    assert(network->sessions[responses_before] == *network_session);
    assert(closed_count == 2U &&
        closed_owners[1] == AIRDAP_DAP_OWNER_NETWORK);

    airdap_dap_service_stats_t stats;
    airdap_dap_service_get_stats(&stats);
    assert(stats.stale_requests == 1U);
}

static void test_queue_full_timeout_reset_and_delivery_failure(
    response_capture_t *usb,
    response_capture_t *network,
    airdap_dap_session_id_t usb_session,
    airdap_dap_session_id_t network_session)
{
    uint8_t maximum_request[AIRDAP_DAP_PACKET_SIZE] = {0x50U};
    uint8_t oversized_request[AIRDAP_DAP_PACKET_SIZE + 1U] = {0x51U};

    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session,
        oversized_request,
        sizeof(oversized_request),
        0U,
        capture_response,
        usb) == AIRDAP_DAP_SERVICE_INVALID_ARGUMENT);

    for (uintptr_t token = 1U; token <= FAKE_QUEUE_CAPACITY; ++token) {
        assert(airdap_dap_service_submit(
            AIRDAP_DAP_TRANSPORT_USB,
            usb_session,
            maximum_request,
            sizeof(maximum_request),
            token,
            capture_response,
            usb) == AIRDAP_DAP_SERVICE_OK);
    }
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session,
        maximum_request,
        sizeof(maximum_request),
        5U,
        capture_response,
        usb) == AIRDAP_DAP_SERVICE_QUEUE_FULL);
    for (size_t index = 0U; index < FAKE_QUEUE_CAPACITY; ++index) {
        process_one();
    }

    assert(airdap_dap_service_session_reset(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session) == AIRDAP_DAP_SERVICE_OK);
    const uint8_t reset_request[] = {0x52U};
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session,
        reset_request,
        sizeof(reset_request),
        6U,
        capture_response,
        usb) == AIRDAP_DAP_SERVICE_OK);
    process_one();
    assert(closed_count == 3U && closed_owners[2] == AIRDAP_DAP_OWNER_USB);

    const uint8_t timed_request[] = {0x53U};
    fake_time_us = 10U;
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session,
        timed_request,
        sizeof(timed_request),
        7U,
        capture_response,
        usb) == AIRDAP_DAP_SERVICE_OK);
    fake_time_us += AIRDAP_DAP_SERVICE_REQUEST_TIMEOUT_US;
    process_one();

    network->accept = false;
    const uint8_t rejected_response_request[] = {0x54U};
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        network_session,
        rejected_response_request,
        sizeof(rejected_response_request),
        8U,
        capture_response,
        network) == AIRDAP_DAP_SERVICE_OK);
    process_one();
    network->accept = true;

    airdap_dap_service_stats_t stats;
    airdap_dap_service_get_stats(&stats);
    assert(stats.queue_full == 1U);
    assert(stats.timed_out == 1U);
    assert(stats.delivery_failures == 1U);
    assert(stats.responses_delivered + stats.delivery_failures ==
        usb->count + network->count);
}

static void test_close_waits_for_started_callback(
    airdap_dap_session_id_t *usb_session)
{
    close_barrier_t barrier = {
        .transport = AIRDAP_DAP_TRANSPORT_USB,
        .session = *usb_session,
    };
    assert(pthread_mutex_init(&barrier.mutex, NULL) == 0);
    assert(pthread_cond_init(&barrier.condition, NULL) == 0);
    const uint8_t request[] = {0x55U};
    assert(airdap_dap_service_submit(
        barrier.transport,
        barrier.session,
        request,
        sizeof(request),
        9U,
        close_during_callback,
        &barrier) == AIRDAP_DAP_SERVICE_OK);

    process_one();
    assert(pthread_join(barrier.close_thread, NULL) == 0);
    assert(barrier.close_returned &&
        barrier.result == AIRDAP_DAP_SERVICE_OK);
    assert(pthread_mutex_lock(&observed_close_mutex) == 0);
    observed_close = NULL;
    assert(pthread_mutex_unlock(&observed_close_mutex) == 0);
    assert(pthread_cond_destroy(&barrier.condition) == 0);
    assert(pthread_mutex_destroy(&barrier.mutex) == 0);

    assert(airdap_dap_service_session_open(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session) == AIRDAP_DAP_SERVICE_OK);
}

int main(void)
{
    response_capture_t usb = {.accept = true};
    response_capture_t network = {.accept = true};
    airdap_dap_session_id_t usb_session = 0U;
    airdap_dap_session_id_t network_session = 0U;

    assert(airdap_dap_service_init(NULL, "test") == ESP_ERR_INVALID_ARG);
    assert(airdap_dap_service_init("ADP-TEST", "test") == ESP_OK);
    assert(dap_init_calls == 1U && worker_task != NULL);
    assert(airdap_dap_service_init("ADP-TEST", "test") ==
        ESP_ERR_INVALID_STATE);

    test_interleaved_transport_routing(
        &usb, &network, &usb_session, &network_session);
    test_closed_inflight_response_is_not_delivered(&usb, &usb_session);
    test_closed_queued_request_is_discarded(&network, &network_session);
    test_queue_full_timeout_reset_and_delivery_failure(
        &usb, &network, usb_session, network_session);
    test_close_waits_for_started_callback(&usb_session);

    assert(airdap_dap_service_session_close(
        AIRDAP_DAP_TRANSPORT_USB,
        usb_session + 1U) == AIRDAP_DAP_SERVICE_STALE_SESSION);
    assert(airdap_dap_service_submit(
        AIRDAP_DAP_TRANSPORT_NETWORK,
        network_session + 1U,
        (const uint8_t[]) {0x60U},
        1U,
        0U,
        capture_response,
        &network) == AIRDAP_DAP_SERVICE_STALE_SESSION);

    puts("DAP service routing tests passed");
    return 0;
}
