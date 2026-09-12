#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airdap_dap_service.h"
#include "airdap_board.h"
#include "airdap_device_identity.h"
#include "airdap_frame.h"
#include "airdap_mode_state.h"
#include "airdap_network_auth.h"
#include "airdap_network_dap.h"
#include "airdap_network_uart.h"
#include "airdap_network_ota.h"
#include "airdap_network_dap_internal.h"
#include "esp_tls.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    TEST_CLIENT_FD = 10,
    TEST_LISTENER_FD = 100,
    TEST_FRAME_BUFFER_SIZE = 4096,
};

struct airdap_network_auth_connection {
    int socket_fd;
};

struct fake_queue {
    size_t item_size;
    size_t capacity;
    size_t count;
    size_t head;
    size_t tail;
    uint8_t *items;
};

struct fake_semaphore {
    pthread_mutex_t mutex;
};

static struct airdap_network_auth_connection auth_connection;
static const airdap_device_identity_t identity = {
    .usb_serial = "ADP-001122334455",
    .device_id = "ADP-001122334455",
    .uuid = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    },
    .firmware_version = "test-fw",
    .protocol_version = AIRDAP_FRAME_PROTOCOL_VERSION,
    .capabilities = UINT32_C(0x01020304),
};

static uint8_t tls_input[TEST_FRAME_BUFFER_SIZE];
static size_t tls_input_size;
static size_t tls_input_offset;
static size_t tls_read_chunk = SIZE_MAX;
static uint8_t tls_output[TEST_FRAME_BUFFER_SIZE];
static size_t tls_output_size;
static int64_t now_us;
static airdap_network_auth_result_t tls_accept_result;
static airdap_network_auth_result_t bind_result;
static airdap_network_auth_result_t validate_result;
static airdap_dap_service_result_t dap_open_result;
static airdap_dap_service_result_t dap_submit_result;
static airdap_mode_dap_result_t admission_result;
static bool deliver_dap_response;
static uint32_t revoke_session_id_at_end;
static bool revoke_during_submit;
static bool timeout_at_end_of_input;
static bool socket_shutdown;
static uint8_t dap_response[64];
static size_t dap_response_size;
static unsigned int auth_bind_calls;
static unsigned int auth_validate_calls;
static unsigned int auth_close_calls;
static unsigned int dap_open_calls;
static unsigned int dap_close_calls;
static unsigned int dap_submit_calls;
static unsigned int shutdown_calls;
static unsigned int socket_close_calls;
static unsigned int task_create_calls;
static unsigned int listener_bind_calls;
static unsigned int queue_timeout_count;
static airdap_network_auth_revoke_fn revoke_handler;
static void *revoke_context;
static airdap_dap_response_fn saved_response_callback;
static void *saved_response_context;
static airdap_dap_response_token_t saved_response_token;
static bool callback_delivered_during_close;
static atomic_uint registry_lock_take_calls;
static pthread_mutex_t connection_pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t connection_pause_condition = PTHREAD_COND_INITIALIZER;
static bool pause_listener_start;
static bool listener_start_paused;
static bool allow_listener_start;
static bool pause_tls_accept;
static bool tls_accept_paused;
static bool allow_tls_accept;
static bool pause_first_tls_read;
static bool first_tls_read_paused;
static bool allow_first_tls_read;
static bool pause_dap_open;
static bool dap_open_paused;
static bool allow_dap_open;
static unsigned int registry_locks_until_pause;
static bool registry_lock_paused;
static bool allow_registry_mutation;
static bool observe_registry_waiter;
static bool registry_waiter_observed;
static unsigned int pause_after_tls_write_call;
static bool tls_write_paused;
static bool allow_tls_write;
static unsigned int tls_write_calls;
static unsigned int board_calls;
static bool board_value;
static uint8_t board_opcode;
static esp_err_t board_result;
static bool control_operation_active;
static bool revoke_before_control_io;
static bool revoke_after_control_io;

airdap_mode_dap_result_t airdap_mode_state_control_operation_begin(
    bool authenticated, airdap_dap_ownership_operation_t *operation)
{
    assert(authenticated && operation != NULL);
    if (admission_result != AIRDAP_MODE_DAP_ALLOWED) {
        return admission_result;
    }
    operation->active = true;
    control_operation_active = true;
    if (revoke_before_control_io) {
        validate_result = AIRDAP_NETWORK_AUTH_UNAUTHENTICATED;
    }
    return AIRDAP_MODE_DAP_ALLOWED;
}

void airdap_dap_ownership_operation_end(airdap_dap_ownership_operation_t *operation)
{
    assert(operation->active && control_operation_active);
    operation->active = false;
    control_operation_active = false;
}

static esp_err_t board_operation(uint8_t opcode, bool value)
{
    assert(control_operation_active && auth_validate_calls >= 3U);
    ++board_calls;
    board_opcode = opcode;
    board_value = value;
    if (revoke_after_control_io) {
        validate_result = AIRDAP_NETWORK_AUTH_UNAUTHENTICATED;
    }
    return board_result;
}

esp_err_t airdap_target_reset_set_asserted(bool asserted)
{
    return board_operation(0x20U, asserted);
}

esp_err_t airdap_target_power_set_allowed(bool allowed)
{
    return board_operation(0x21U, allowed);
}

esp_err_t airdap_target_power_get_active(bool *active)
{
    *active = false;
    return board_operation(0x22U, false);
}

static unsigned int ota_dispatch_calls, ota_disconnect_calls, ota_reboot_calls;
static unsigned int uart_open_calls, uart_close_calls, uart_write_calls;

airdap_frame_error_code_t airdap_network_ota_dispatch(
    airdap_network_auth_connection_t *connection, uint32_t session,
    const uint8_t *request, size_t request_size,
    uint8_t *response, size_t capacity, size_t *size)
{
    assert(connection == &auth_connection && session == 77);
    assert(request_size > 0 && capacity >= AIRDAP_NETWORK_OTA_MAX_RESPONSE);
    ++ota_dispatch_calls;
    response[0] = request[0]; response[1] = 0; *size = 2;
    return AIRDAP_FRAME_ERROR_NONE;
}
void airdap_network_ota_disconnect(airdap_network_auth_connection_t *connection, uint32_t session)
{
    if (connection && session) {
        assert(connection == &auth_connection && session == 77);
        assert(auth_close_calls == 0);
        ++ota_disconnect_calls;
    }
}
void airdap_network_ota_reboot(airdap_network_auth_connection_t *connection, uint32_t session)
{
    assert(connection == &auth_connection && session == 77);
    assert(tls_output_size >= 2 && tls_output[tls_output_size - 2] == 0x35 &&
        tls_output[tls_output_size - 1] == 0);
    ++ota_reboot_calls;
}

static bool uart_live;
static unsigned int fail_validation_at;
static size_t block_write_after;

static void reset_connection_fakes(void)
{
    ota_dispatch_calls = ota_disconnect_calls = ota_reboot_calls = 0;
    uart_open_calls = uart_close_calls = uart_write_calls = 0;
    uart_live = false;
    fail_validation_at = 0;
    block_write_after = SIZE_MAX;
    tls_input_size = 0U;
    tls_input_offset = 0U;
    tls_read_chunk = SIZE_MAX;
    tls_output_size = 0U;
    now_us = 0;
    tls_accept_result = AIRDAP_NETWORK_AUTH_OK;
    bind_result = AIRDAP_NETWORK_AUTH_OK;
    validate_result = AIRDAP_NETWORK_AUTH_OK;
    dap_open_result = AIRDAP_DAP_SERVICE_OK;
    dap_submit_result = AIRDAP_DAP_SERVICE_OK;
    admission_result = AIRDAP_MODE_DAP_ALLOWED;
    deliver_dap_response = true;
    revoke_session_id_at_end = 0U;
    revoke_during_submit = false;
    timeout_at_end_of_input = false;
    socket_shutdown = false;
    dap_response[0] = 0x00U;
    dap_response[1] = 0x08U;
    memcpy(dap_response + 2U, "test-fw\0", 8U);
    dap_response_size = 10U;
    auth_bind_calls = 0U;
    auth_validate_calls = 0U;
    auth_close_calls = 0U;
    dap_open_calls = 0U;
    dap_close_calls = 0U;
    dap_submit_calls = 0U;
    shutdown_calls = 0U;
    socket_close_calls = 0U;
    queue_timeout_count = 0U;
    saved_response_callback = NULL;
    saved_response_context = NULL;
    saved_response_token = 0U;
    callback_delivered_during_close = false;
    pause_tls_accept = false;
    tls_accept_paused = false;
    allow_tls_accept = false;
    pause_first_tls_read = false;
    first_tls_read_paused = false;
    allow_first_tls_read = false;
    pause_dap_open = false;
    dap_open_paused = false;
    allow_dap_open = false;
    registry_locks_until_pause = 0U;
    registry_lock_paused = false;
    allow_registry_mutation = false;
    observe_registry_waiter = false;
    registry_waiter_observed = false;
    pause_after_tls_write_call = 0U;
    tls_write_paused = false;
    allow_tls_write = false;
    tls_write_calls = 0U;
    board_calls = 0U;
    board_result = ESP_OK;
    control_operation_active = false;
    revoke_before_control_io = false;
    revoke_after_control_io = false;
}

static void append_request(
    uint8_t type,
    uint32_t session_id,
    uint32_t sequence,
    const uint8_t *payload,
    size_t payload_size)
{
    assert(payload_size <= UINT16_MAX);
    airdap_frame_header_t header = {
        .magic = AIRDAP_FRAME_MAGIC,
        .version = AIRDAP_FRAME_PROTOCOL_VERSION,
        .type = type,
        .session_id = session_id,
        .sequence = sequence,
        .payload_length = (uint16_t) payload_size,
    };
    size_t encoded_size = 0U;
    assert(airdap_frame_encode(
        &header,
        payload,
        tls_input + tls_input_size,
        sizeof(tls_input) - tls_input_size,
        &encoded_size) == AIRDAP_FRAME_ERROR_NONE);
    tls_input_size += encoded_size;
}

static void append_oversized_dap_header(
    uint32_t session_id,
    uint32_t sequence,
    uint16_t payload_size)
{
    assert(tls_input_size + AIRDAP_FRAME_HEADER_SIZE <= sizeof(tls_input));
    uint8_t *header = tls_input + tls_input_size;
    memset(header, 0, AIRDAP_FRAME_HEADER_SIZE);
    memcpy(header, "ADAP", 4U);
    header[4] = AIRDAP_FRAME_PROTOCOL_VERSION;
    header[5] = AIRDAP_FRAME_TYPE_DAP_REQUEST;
    header[8] = (uint8_t) (session_id >> 24U);
    header[9] = (uint8_t) (session_id >> 16U);
    header[10] = (uint8_t) (session_id >> 8U);
    header[11] = (uint8_t) session_id;
    header[12] = (uint8_t) (sequence >> 24U);
    header[13] = (uint8_t) (sequence >> 16U);
    header[14] = (uint8_t) (sequence >> 8U);
    header[15] = (uint8_t) sequence;
    header[16] = (uint8_t) (payload_size >> 8U);
    header[17] = (uint8_t) payload_size;
    tls_input_size += AIRDAP_FRAME_HEADER_SIZE;
}

static airdap_frame_header_t next_response(
    size_t *offset,
    const uint8_t **payload)
{
    airdap_frame_header_t header = {0};
    size_t frame_size = 0U;
    airdap_frame_error_code_t error = AIRDAP_FRAME_ERROR_INTERNAL;
    assert(airdap_frame_decode(
        tls_output + *offset,
        tls_output_size - *offset,
        &header,
        payload,
        &frame_size,
        &error) == AIRDAP_FRAME_DECODE_OK);
    assert(error == AIRDAP_FRAME_ERROR_NONE);
    *offset += frame_size;
    return header;
}

static void assert_error_payload(
    const uint8_t *payload,
    size_t payload_size,
    airdap_frame_error_code_t expected)
{
    airdap_frame_error_code_t decoded = AIRDAP_FRAME_ERROR_NONE;
    airdap_frame_error_code_t parse_error = AIRDAP_FRAME_ERROR_INTERNAL;
    assert(airdap_frame_error_code_decode(
        payload,
        payload_size,
        &decoded,
        &parse_error) == AIRDAP_FRAME_DECODE_OK);
    assert(parse_error == AIRDAP_FRAME_ERROR_NONE);
    assert(decoded == expected);
}

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return &identity;
}

int64_t esp_timer_get_time(void)
{
    return now_us;
}

esp_err_t airdap_network_auth_set_revoke_handler(
    airdap_network_auth_revoke_fn handler,
    void *context)
{
    revoke_handler = handler;
    revoke_context = context;
    return ESP_OK;
}

airdap_network_auth_result_t airdap_network_auth_tls_accept(
    int socket_fd,
    airdap_network_auth_connection_t **connection)
{
    if (tls_accept_result != AIRDAP_NETWORK_AUTH_OK) {
        *connection = NULL;
        return tls_accept_result;
    }
    if (pause_tls_accept) {
        assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
        tls_accept_paused = true;
        assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
        while (!allow_tls_accept) {
            assert(pthread_cond_wait(
                &connection_pause_condition,
                &connection_pause_mutex) == 0);
        }
        assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    }
    auth_connection.socket_fd = socket_fd;
    *connection = &auth_connection;
    return AIRDAP_NETWORK_AUTH_OK;
}

ssize_t airdap_network_auth_tls_read(
    airdap_network_auth_connection_t *connection,
    void *buffer,
    size_t length)
{
    assert(connection == &auth_connection);
    if (pause_first_tls_read) {
        assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
        pause_first_tls_read = false;
        first_tls_read_paused = true;
        assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
        while (!allow_first_tls_read) {
            assert(pthread_cond_wait(
                &connection_pause_condition,
                &connection_pause_mutex) == 0);
        }
        assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    }
    if (tls_input_offset == tls_input_size) {
        if (revoke_session_id_at_end != 0U) {
            const uint32_t revoked_session_id = revoke_session_id_at_end;
            revoke_session_id_at_end = 0U;
            assert(revoke_handler != NULL);
            revoke_handler(revoke_context, revoked_session_id);
            airdap_network_dap_process_revocations();
        }
        if (timeout_at_end_of_input) {
            return ESP_TLS_ERR_SSL_WANT_READ;
        }
        return 0;
    }
    const size_t available = tls_input_size - tls_input_offset;
    size_t copied = length < available ? length : available;
    if (copied > tls_read_chunk) {
        copied = tls_read_chunk;
    }
    memcpy(buffer, tls_input + tls_input_offset, copied);
    tls_input_offset += copied;
    return (ssize_t) copied;
}

ssize_t airdap_network_auth_tls_write(
    airdap_network_auth_connection_t *connection,
    const void *buffer,
    size_t length)
{
    assert(connection == &auth_connection);
    if (socket_shutdown) {
        return -1;
    }
    if (tls_output_size >= block_write_after) return ESP_TLS_ERR_SSL_WANT_WRITE;
    assert(tls_output_size + length <= sizeof(tls_output));
    memcpy(tls_output + tls_output_size, buffer, length);
    tls_output_size += length;
    ++tls_write_calls;
    if (pause_after_tls_write_call == tls_write_calls) {
        assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
        tls_write_paused = true;
        assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
        while (!allow_tls_write) {
            assert(pthread_cond_wait(
                &connection_pause_condition,
                &connection_pause_mutex) == 0);
        }
        assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    }
    return (ssize_t) length;
}

airdap_network_auth_result_t airdap_network_auth_session_bind(
    airdap_network_auth_connection_t *connection,
    const uint8_t *owner_token,
    size_t owner_token_size,
    airdap_network_auth_session_info_t *session_info)
{
    assert(connection == &auth_connection);
    assert(owner_token == NULL);
    assert(owner_token_size == 0U);
    ++auth_bind_calls;
    if (bind_result != AIRDAP_NETWORK_AUTH_OK) {
        return bind_result;
    }
    memset(session_info, 0, sizeof(*session_info));
    session_info->session_id = 77U;
    for (size_t index = 0U; index < sizeof(session_info->session_token); ++index) {
        session_info->session_token[index] = (uint8_t) index;
    }
    return AIRDAP_NETWORK_AUTH_OK;
}

airdap_network_auth_result_t airdap_network_auth_session_validate(
    airdap_network_auth_connection_t *connection,
    uint32_t session_id)
{
    assert(connection == &auth_connection);
    assert(session_id == 77U);
    ++auth_validate_calls;
    if (fail_validation_at != 0 && auth_validate_calls >= fail_validation_at)
        return AIRDAP_NETWORK_AUTH_EXPIRED;
    return validate_result;
}

void airdap_network_auth_connection_close(
    airdap_network_auth_connection_t *connection)
{
    assert(connection == &auth_connection);
    ++auth_close_calls;
}

airdap_dap_service_result_t airdap_dap_service_session_open(
    airdap_dap_transport_t transport,
    bool authenticated,
    airdap_dap_session_id_t *session)
{
    assert(transport == AIRDAP_DAP_TRANSPORT_NETWORK);
    assert(authenticated);
    ++dap_open_calls;
    if (pause_dap_open) {
        assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
        dap_open_paused = true;
        assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
        while (!allow_dap_open) {
            assert(pthread_cond_wait(
                &connection_pause_condition,
                &connection_pause_mutex) == 0);
        }
        assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    }
    if (dap_open_result == AIRDAP_DAP_SERVICE_OK) {
        *session = 55U;
    }
    return dap_open_result;
}

airdap_dap_service_result_t airdap_dap_service_session_close(
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session)
{
    assert(transport == AIRDAP_DAP_TRANSPORT_NETWORK);
    assert(session == 55U);
    ++dap_close_calls;
    if (saved_response_callback != NULL) {
        callback_delivered_during_close = saved_response_callback(
            saved_response_context,
            transport,
            session,
            saved_response_token,
            dap_response,
            dap_response_size);
        saved_response_callback = NULL;
    }
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
    assert(transport == AIRDAP_DAP_TRANSPORT_NETWORK);
    assert(session == 55U);
    assert(request != NULL);
    assert(request_length > 0U);
    ++dap_submit_calls;
    saved_response_callback = response_callback;
    saved_response_context = response_context;
    saved_response_token = response_token;
    if (revoke_during_submit) {
        revoke_during_submit = false;
        assert(revoke_handler != NULL);
        revoke_handler(revoke_context, 77U);
        airdap_network_dap_process_revocations();
        return dap_submit_result;
    }
    if (dap_submit_result == AIRDAP_DAP_SERVICE_OK && deliver_dap_response) {
        assert(response_callback(
            response_context,
            transport,
            session,
            response_token,
            dap_response,
            dap_response_size));
        saved_response_callback = NULL;
    }
    return dap_submit_result;
}

airdap_mode_dap_result_t airdap_mode_state_dap_admission(
    airdap_dap_owner_t requested_owner,
    bool authenticated)
{
    assert(requested_owner == AIRDAP_DAP_OWNER_NETWORK);
    assert(authenticated);
    return admission_result;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    assert(length > 0U && item_size > 0U);
    struct fake_queue *queue = calloc(1U, sizeof(*queue));
    assert(queue != NULL);
    queue->items = malloc((size_t) length * item_size);
    assert(queue->items != NULL);
    queue->item_size = item_size;
    queue->capacity = length;
    return queue;
}

BaseType_t xQueueSend(
    QueueHandle_t queue,
    const void *item,
    TickType_t ticks_to_wait)
{
    assert(queue != NULL && item != NULL && ticks_to_wait == 0U);
    if (queue->count == queue->capacity) {
        return pdFALSE;
    }
    memcpy(
        queue->items + queue->tail * queue->item_size,
        item,
        queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->capacity;
    ++queue->count;
    return pdTRUE;
}

BaseType_t xQueueReceive(
    QueueHandle_t queue,
    void *item,
    TickType_t ticks_to_wait)
{
    assert(queue != NULL && item != NULL);
    if (queue->count == 0U) {
        if (ticks_to_wait == 0U) {
            return pdFALSE;
        }
        assert(ticks_to_wait == pdMS_TO_TICKS(
            AIRDAP_DAP_SERVICE_REQUEST_TIMEOUT_US / 1000U));
        ++queue_timeout_count;
        now_us += AIRDAP_DAP_SERVICE_REQUEST_TIMEOUT_US;
        return pdFALSE;
    }
    memcpy(
        item,
        queue->items + queue->head * queue->item_size,
        queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t queue)
{
    assert(queue != NULL);
    free(queue->items);
    free(queue);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_semaphore *semaphore = calloc(1U, sizeof(*semaphore));
    assert(semaphore != NULL);
    assert(pthread_mutex_init(&semaphore->mutex, NULL) == 0);
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore != NULL && timeout == portMAX_DELAY);
    atomic_fetch_add(&registry_lock_take_calls, 1U);
    assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
    if (observe_registry_waiter) {
        observe_registry_waiter = false;
        registry_waiter_observed = true;
        assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
    }
    assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);

    if (pthread_mutex_lock(&semaphore->mutex) != 0) {
        return pdFALSE;
    }
    assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
    if (registry_locks_until_pause > 0U &&
        --registry_locks_until_pause == 0U) {
        registry_lock_paused = true;
        assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
        while (!allow_registry_mutation) {
            assert(pthread_cond_wait(
                &connection_pause_condition,
                &connection_pause_mutex) == 0);
        }
    }
    assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL);
    return pthread_mutex_unlock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xTaskCreate(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle)
{
    assert(task != NULL && name != NULL && stack_depth > 0U && priority > 0U);
    (void) argument;
    (void) handle;
    ++task_create_calls;
    if (pause_listener_start && strcmp(name, "dap_tcp_listener") == 0) {
        assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
        listener_start_paused = true;
        assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
        while (!allow_listener_start) {
            assert(pthread_cond_wait(
                &connection_pause_condition,
                &connection_pause_mutex) == 0);
        }
        assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    }
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    (void) task;
}

int socket(int domain, int type, int protocol)
{
    assert(domain == AF_INET && type == SOCK_STREAM);
    (void) protocol;
    return TEST_LISTENER_FD;
}

int setsockopt(
    int socket_fd,
    int level,
    int option_name,
    const void *option_value,
    socklen_t option_length)
{
    assert(socket_fd == TEST_LISTENER_FD);
    assert(level == SOL_SOCKET && option_name == SO_REUSEADDR);
    assert(option_value != NULL && option_length == sizeof(int));
    return 0;
}

int bind(int socket_fd, const struct sockaddr *address, socklen_t address_length)
{
    assert(socket_fd == TEST_LISTENER_FD);
    assert(address != NULL && address_length == sizeof(struct sockaddr_in));
    const struct sockaddr_in *bound = (const struct sockaddr_in *) address;
    assert(ntohs(bound->sin_port) == (listener_bind_calls == 0 ? 3260 : 3261));
    ++listener_bind_calls;
    return 0;
}

int listen(int socket_fd, int backlog)
{
    assert(socket_fd == TEST_LISTENER_FD);
    assert(backlog == AIRDAP_NETWORK_DAP_MAX_CONNECTIONS);
    return 0;
}

int accept(int socket_fd, struct sockaddr *address, socklen_t *address_length)
{
    assert(socket_fd == TEST_LISTENER_FD);
    (void) address;
    (void) address_length;
    errno = EAGAIN;
    return -1;
}

int shutdown(int socket_fd, int how)
{
    assert(socket_fd == TEST_CLIENT_FD && how == SHUT_RDWR);
    ++shutdown_calls;
    socket_shutdown = true;
    return 0;
}

int close(int socket_fd)
{
    assert(socket_fd == TEST_CLIENT_FD || socket_fd == TEST_LISTENER_FD);
    if (socket_fd == TEST_CLIENT_FD) {
        ++socket_close_calls;
    }
    return 0;
}

int fcntl(int socket_fd, int command, ...)
{
    assert(socket_fd == TEST_CLIENT_FD || socket_fd == TEST_LISTENER_FD);
    if (command == F_GETFL) {
        return O_NONBLOCK;
    }
    assert(command == F_SETFL);
    va_list arguments;
    va_start(arguments, command);
    const int flags = va_arg(arguments, int);
    va_end(arguments);
    assert((flags & O_NONBLOCK) != 0);
    return 0;
}

int poll(struct pollfd *descriptors, nfds_t count, int timeout_ms)
{
    assert(descriptors != NULL && count == 1U && timeout_ms >= 0);
    if ((timeout_at_end_of_input || block_write_after != SIZE_MAX) && descriptors[0].fd == TEST_CLIENT_FD) {
        now_us += (int64_t) timeout_ms * 1000;
        return 0;
    }
    descriptors[0].revents = descriptors[0].events;
    return 1;
}

static void assert_network_dap_status(
    bool listener_ready,
    unsigned int allocated_connections,
    unsigned int tls_connections,
    unsigned int authenticated_connections,
    unsigned int dap_sessions)
{
    airdap_network_dap_status_t status = {0};
    const unsigned int locks_before = atomic_load(&registry_lock_take_calls);
    assert(airdap_network_dap_get_status(&status) == ESP_OK);
    assert(atomic_load(&registry_lock_take_calls) == locks_before + 1U);
    assert(status.listener_ready == listener_ready);
    assert(status.allocated_connections == allocated_connections);
    assert(status.tls_connections == tls_connections);
    assert(status.authenticated_connections == authenticated_connections);
    assert(status.dap_sessions == dap_sessions);
}

static void wait_for_pause(bool *paused)
{
    assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
    while (!*paused) {
        assert(pthread_cond_wait(
            &connection_pause_condition,
            &connection_pause_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
}

static void release_pause(bool *allow)
{
    assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
    *allow = true;
    assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
    assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
}

typedef struct {
    esp_err_t result;
} start_thread_arguments_t;

typedef struct {
    esp_err_t result;
    airdap_network_dap_status_t status;
    atomic_bool completed;
} status_thread_arguments_t;

static void *start_component_worker(void *argument)
{
    start_thread_arguments_t *start = argument;
    start->result = airdap_network_dap_start();
    return NULL;
}

static void *status_worker(void *argument)
{
    status_thread_arguments_t *observed = argument;
    observed->result = airdap_network_dap_get_status(&observed->status);
    atomic_store(&observed->completed, true);
    return NULL;
}

static void *handle_socket_worker(void *argument)
{
    const int socket_fd = *(const int *) argument;
    airdap_network_dap_handle_socket(socket_fd);
    return NULL;
}

static void test_status_is_empty_during_listener_startup(void)
{
    pause_listener_start = true;
    start_thread_arguments_t start = {.result = ESP_FAIL};
    pthread_t start_thread;
    assert(pthread_create(
        &start_thread,
        NULL,
        start_component_worker,
        &start) == 0);
    wait_for_pause(&listener_start_paused);

    airdap_network_dap_status_t status;
    memset(&status, 0xA5, sizeof(status));
    const unsigned int locks_before = atomic_load(&registry_lock_take_calls);
    assert(airdap_network_dap_get_status(&status) == ESP_OK);
    assert(atomic_load(&registry_lock_take_calls) == locks_before);
    const airdap_network_dap_status_t empty = {0};
    assert(memcmp(&status, &empty, sizeof(status)) == 0);

    release_pause(&allow_listener_start);
    assert(pthread_join(start_thread, NULL) == 0);
    assert(start.result == ESP_OK);
    assert(task_create_calls == 1U);
    assert(revoke_handler != NULL);
    assert_network_dap_status(true, 0U, 0U, 0U, 0U);
}

static void test_concurrent_status_tracks_connection_lifecycle(void)
{
    reset_connection_fakes();
    append_request(AIRDAP_FRAME_TYPE_HELLO, 8U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 8U, 2U, NULL, 0U);
    pause_tls_accept = true;
    pause_first_tls_read = true;
    pause_dap_open = true;
    pause_after_tls_write_call = 2U;

    int socket_fd = TEST_CLIENT_FD;
    pthread_t connection_thread;
    assert(pthread_create(
        &connection_thread,
        NULL,
        handle_socket_worker,
        &socket_fd) == 0);

    wait_for_pause(&tls_accept_paused);
    assert_network_dap_status(true, 1U, 0U, 0U, 0U);
    release_pause(&allow_tls_accept);

    wait_for_pause(&first_tls_read_paused);
    assert_network_dap_status(true, 1U, 1U, 0U, 0U);

    assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
    registry_locks_until_pause = 2U;
    allow_first_tls_read = true;
    assert(pthread_cond_broadcast(&connection_pause_condition) == 0);
    assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    wait_for_pause(&registry_lock_paused);

    status_thread_arguments_t observed = {0};
    atomic_init(&observed.completed, false);
    assert(pthread_mutex_lock(&connection_pause_mutex) == 0);
    observe_registry_waiter = true;
    assert(pthread_mutex_unlock(&connection_pause_mutex) == 0);
    pthread_t status_thread;
    assert(pthread_create(
        &status_thread,
        NULL,
        status_worker,
        &observed) == 0);
    wait_for_pause(&registry_waiter_observed);
    assert(!atomic_load(&observed.completed));
    release_pause(&allow_registry_mutation);

    wait_for_pause(&dap_open_paused);
    assert(pthread_join(status_thread, NULL) == 0);
    assert(observed.result == ESP_OK);
    assert(observed.status.listener_ready);
    assert(observed.status.allocated_connections == 1U);
    assert(observed.status.tls_connections == 1U);
    assert(observed.status.authenticated_connections == 1U);
    assert(observed.status.dap_sessions == 0U);
    release_pause(&allow_dap_open);

    wait_for_pause(&tls_write_paused);
    assert_network_dap_status(true, 1U, 1U, 1U, 1U);
    release_pause(&allow_tls_write);
    assert(pthread_join(connection_thread, NULL) == 0);

    assert_network_dap_status(true, 0U, 0U, 0U, 0U);
}

static void test_uninitialized_status_is_empty(void)
{
    airdap_network_dap_status_t status;
    memset(&status, 0xA5, sizeof(status));
    assert(airdap_network_dap_get_status(NULL) == ESP_ERR_INVALID_ARG);
    assert(airdap_network_dap_get_status(&status) == ESP_OK);
    const airdap_network_dap_status_t empty = {0};
    assert(memcmp(&status, &empty, sizeof(status)) == 0);
}

static void test_authenticated_hello_dap_info_and_keepalive(void)
{
    reset_connection_fakes();
    tls_read_chunk = 3U;
    static const uint8_t dap_info[] = {0x00U, 0x09U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 9U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 9U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 9U, 3U,
        dap_info, sizeof(dap_info));
    append_request(AIRDAP_FRAME_TYPE_KEEPALIVE, 9U, 4U, NULL, 0U);

    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(auth_bind_calls == 1U);
    assert(auth_validate_calls == 4U);
    assert(dap_open_calls == 1U && dap_submit_calls == 1U);
    assert(dap_close_calls == 1U && auth_close_calls == 1U);
    assert(socket_close_calls == 1U);

    size_t offset = 0U;
    const uint8_t *payload = NULL;
    airdap_frame_header_t response = next_response(&offset, &payload);
    assert(response.type == AIRDAP_FRAME_TYPE_HELLO);
    assert(response.session_id == 9U && response.sequence == 1U);
    assert(response.payload_length == AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE +
        strlen(identity.firmware_version));
    assert(memcmp(payload, identity.uuid, sizeof(identity.uuid)) == 0);
    assert(memcmp(payload + 20U, identity.device_id,
        AIRDAP_DEVICE_SERIAL_LENGTH) == 0);
    assert(memcmp(payload + AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE,
        identity.firmware_version, strlen(identity.firmware_version)) == 0);

    response = next_response(&offset, &payload);
    assert(response.type == AIRDAP_FRAME_TYPE_AUTH);
    assert(response.payload_length == AIRDAP_NETWORK_DAP_AUTH_RESPONSE_SIZE);
    assert(memcmp(payload, "\x00\x00\x00\x4D", 4U) == 0);
    for (size_t index = 0U; index < AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE;
         ++index) {
        assert(payload[4U + index] == (uint8_t) index);
    }

    response = next_response(&offset, &payload);
    assert(response.type == AIRDAP_FRAME_TYPE_DAP_RESPONSE);
    assert(response.sequence == 3U);
    assert(response.payload_length == dap_response_size);
    assert(memcmp(payload, dap_response, dap_response_size) == 0);

    response = next_response(&offset, &payload);
    assert(response.type == AIRDAP_FRAME_TYPE_KEEPALIVE);
    assert(response.payload_length == 0U);
    assert(offset == tls_output_size);
}

static void test_dap_before_auth_is_rejected_without_dispatch(void)
{
    reset_connection_fakes();
    static const uint8_t dap_info[] = {0x00U, 0x09U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 10U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 10U, 2U,
        dap_info, sizeof(dap_info));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(auth_bind_calls == 0U && dap_submit_calls == 0U);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert(error.type == AIRDAP_FRAME_TYPE_ERROR && error.sequence == 2U);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
}

static void test_non_hello_first_frame_is_rejected_and_closed(void)
{
    static const uint8_t types[] = {
        AIRDAP_FRAME_TYPE_AUTH,
        AIRDAP_FRAME_TYPE_DAP_REQUEST,
        AIRDAP_FRAME_TYPE_CONTROL_REQUEST,
        AIRDAP_FRAME_TYPE_KEEPALIVE,
    };
    for (size_t index = 0U; index < sizeof(types); ++index) {
        reset_connection_fakes();
        append_request(types[index], 20U + (uint32_t) index, 1U,
            NULL, 0U);
        append_request(AIRDAP_FRAME_TYPE_HELLO,
            20U + (uint32_t) index, 2U, NULL, 0U);
        airdap_network_dap_handle_socket(TEST_CLIENT_FD);

        assert(auth_bind_calls == 0U && dap_open_calls == 0U);
        size_t offset = 0U;
        const uint8_t *payload = NULL;
        const airdap_frame_header_t error = next_response(&offset, &payload);
        assert(error.type == AIRDAP_FRAME_TYPE_ERROR && error.sequence == 1U);
        assert_error_payload(payload, error.payload_length,
            AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
        assert(offset == tls_output_size);
    }
}

static void test_oversized_dap_payload_returns_stable_error(void)
{
    static const uint16_t payload_sizes[] = {509U, 4096U};
    for (size_t index = 0U;
         index < sizeof(payload_sizes) / sizeof(payload_sizes[0]);
         ++index) {
        reset_connection_fakes();
        append_oversized_dap_header(
            30U + (uint32_t) index,
            1U,
            payload_sizes[index]);
        airdap_network_dap_handle_socket(TEST_CLIENT_FD);

        size_t offset = 0U;
        const uint8_t *payload = NULL;
        const airdap_frame_header_t error = next_response(&offset, &payload);
        assert(error.type == AIRDAP_FRAME_TYPE_ERROR && error.sequence == 1U);
        assert_error_payload(payload, error.payload_length,
            AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE);
        assert(offset == tls_output_size);
        assert(auth_bind_calls == 0U && dap_open_calls == 0U);
    }
}

static void test_usb_owner_returns_busy_without_dap_dispatch(void)
{
    reset_connection_fakes();
    admission_result = AIRDAP_MODE_DAP_BUSY;
    static const uint8_t dap_connect[] = {0x02U, 0x01U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 11U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 11U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 11U, 3U,
        dap_connect, sizeof(dap_connect));
    pause_after_tls_write_call = 3U;
    int socket_fd = TEST_CLIENT_FD;
    pthread_t connection_thread;
    assert(pthread_create(
        &connection_thread,
        NULL,
        handle_socket_worker,
        &socket_fd) == 0);
    wait_for_pause(&tls_write_paused);
    assert_network_dap_status(true, 1U, 1U, 1U, 1U);
    release_pause(&allow_tls_write);
    assert(pthread_join(connection_thread, NULL) == 0);

    assert(dap_submit_calls == 0U);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_BUSY);
    assert_network_dap_status(true, 0U, 0U, 0U, 0U);
}

static void test_invalid_auth_payload_never_marks_connection_authenticated(void)
{
    reset_connection_fakes();
    const uint8_t invalid_auth[] = {0x01U};
    static const uint8_t dap_info[] = {0x00U, 0x09U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 15U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 15U, 2U,
        invalid_auth, sizeof(invalid_auth));
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 15U, 3U,
        dap_info, sizeof(dap_info));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(auth_bind_calls == 0U);
    assert(dap_open_calls == 0U && dap_submit_calls == 0U);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert(error.type == AIRDAP_FRAME_TYPE_ERROR && error.sequence == 2U);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
    assert(offset == tls_output_size);
}

static void test_usb_ota_vendor_commands_are_not_exposed_over_dap_tcp(void)
{
    reset_connection_fakes();
    static const uint8_t ota_query[] = {0x80U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 16U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 16U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 16U, 3U,
        ota_query, sizeof(ota_query));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(dap_submit_calls == 0U);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
}

static void test_dap_timeout_discards_late_callback(void)
{
    reset_connection_fakes();
    deliver_dap_response = false;
    static const uint8_t dap_info[] = {0x00U, 0x09U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 12U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 12U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 12U, 3U,
        dap_info, sizeof(dap_info));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(queue_timeout_count == 1U);
    assert(saved_response_callback == NULL);
    assert(!callback_delivered_during_close);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_TIMEOUT);
}

static void test_sequence_error_is_stable_and_not_dispatched(void)
{
    reset_connection_fakes();
    append_request(AIRDAP_FRAME_TYPE_HELLO, 13U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_KEEPALIVE, 13U, 3U, NULL, 0U);
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER);
}

static void test_malformed_header_is_closed_without_trusting_response_fields(void)
{
    reset_connection_fakes();
    append_request(AIRDAP_FRAME_TYPE_HELLO, 17U, 1U, NULL, 0U);
    tls_input[0] = 0x42U;
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(tls_output_size == 0U);
    assert(auth_bind_calls == 0U && dap_open_calls == 0U);
    assert(auth_close_calls == 1U && socket_close_calls == 1U);
}

static void test_revoke_shuts_down_registered_connection(void)
{
    reset_connection_fakes();
    revoke_session_id_at_end = 77U;
    append_request(AIRDAP_FRAME_TYPE_HELLO, 14U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 14U, 2U, NULL, 0U);
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(shutdown_calls == 1U);
    assert(dap_close_calls == 2U && auth_close_calls == 1U);
    assert_network_dap_status(true, 0U, 0U, 0U, 0U);
}

static void test_unrelated_revoke_does_not_shutdown_registered_connection(void)
{
    reset_connection_fakes();
    revoke_session_id_at_end = 88U;
    append_request(AIRDAP_FRAME_TYPE_HELLO, 19U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 19U, 2U, NULL, 0U);
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(shutdown_calls == 0U);
    assert(dap_close_calls == 1U && auth_close_calls == 1U);
}

static void test_revoke_closes_service_before_queued_dap_can_respond(void)
{
    reset_connection_fakes();
    revoke_during_submit = true;
    static const uint8_t dap_info[] = {0x00U, 0x09U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 18U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 18U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 18U, 3U,
        dap_info, sizeof(dap_info));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(shutdown_calls == 1U);
    /* The revoke path and the connection teardown both synchronize through
     * session_close before the response queue is deleted. */
    assert(dap_close_calls == 2U);
    assert(saved_response_callback == NULL);
    assert(callback_delivered_during_close);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    (void) next_response(&offset, &payload);
    assert(offset == tls_output_size);
    assert_network_dap_status(true, 0U, 0U, 0U, 0U);
}

static void test_pre_auth_partial_frame_times_out(void)
{
    reset_connection_fakes();
    timeout_at_end_of_input = true;
    append_request(AIRDAP_FRAME_TYPE_HELLO, 15U, 1U, NULL, 0U);
    tls_input_size -= 1U;
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(tls_output_size == 0U);
    assert(auth_bind_calls == 0U && dap_open_calls == 0U);
    assert(auth_close_calls == 1U && socket_close_calls == 1U);
}

airdap_target_uart_result_t airdap_target_uart_session_open(
    airdap_target_uart_transport_t transport, bool authenticated, uint32_t *session)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_NETWORK && authenticated);
    ++uart_open_calls; uart_live = true; *session = 123;
    return AIRDAP_TARGET_UART_OK;
}
airdap_target_uart_result_t airdap_target_uart_session_close(
    airdap_target_uart_transport_t transport, uint32_t session)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_NETWORK && session == 123);
    ++uart_close_calls; uart_live = false;
    return AIRDAP_TARGET_UART_OK;
}
airdap_target_uart_result_t airdap_target_uart_tx_acquire(
    airdap_target_uart_transport_t transport, uint32_t session)
{
    assert(transport == AIRDAP_TARGET_UART_TRANSPORT_NETWORK && session == 123);
    return uart_live ? AIRDAP_TARGET_UART_OK : AIRDAP_TARGET_UART_STALE_SESSION;
}
airdap_target_uart_result_t airdap_target_uart_session_configure(
    airdap_target_uart_transport_t t, uint32_t s, uint32_t baud, uint8_t stop,
    uint8_t parity, uint8_t bits)
{
    (void)t; (void)s; (void)baud; (void)stop; (void)parity; (void)bits;
    return AIRDAP_TARGET_UART_OK;
}
airdap_target_uart_result_t airdap_target_uart_session_get_status(
    airdap_target_uart_transport_t t, uint32_t s, airdap_target_uart_session_status_t *status)
{
    (void)t; (void)s; memset(status, 0, sizeof(*status));
    return AIRDAP_TARGET_UART_OK;
}
esp_err_t airdap_target_uart_get_status(airdap_target_uart_status_t *status)
{
    memset(status, 0, sizeof(*status)); status->baud_rate = 115200; status->data_bits = 8;
    return ESP_OK;
}
airdap_target_uart_result_t airdap_target_uart_session_read(
    airdap_target_uart_transport_t t, uint32_t s, uint8_t *data, size_t capacity, size_t *received)
{
    (void)t; (void)s; assert(capacity > 0); data[0] = 0xA5; *received = 1;
    return AIRDAP_TARGET_UART_OK;
}
airdap_target_uart_result_t airdap_target_uart_session_write(
    airdap_target_uart_transport_t t, uint32_t s, const uint8_t *data, size_t size, size_t *written)
{
    (void)t; (void)s; assert(data != NULL && uart_live); ++uart_write_calls;
    *written = size; return AIRDAP_TARGET_UART_OK;
}

static void uart_handshake(void)
{
    append_request(AIRDAP_FRAME_TYPE_HELLO, 9, 1, NULL, 0);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 9, 2, NULL, 0);
}

static void test_uart_transport(void)
{
    const uint8_t write[] = {0x13, 0xA5};
    const uint8_t read[] = {0x14, 0, 1};
    for (size_t chunk = 1; chunk <= 4096; chunk *= 16) {
        reset_connection_fakes(); tls_read_chunk = chunk;
        uart_handshake();
        append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, 3, write, 2);
        append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, 3, write, 2);
        append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, 4, read, 3);
        airdap_network_uart_handle_socket(TEST_CLIENT_FD);
        assert(uart_open_calls == 1 && uart_close_calls == 1 && uart_write_calls == 1);
        assert(dap_open_calls == 0 && dap_submit_calls == 0 && !uart_live);
        size_t offset = 0; const uint8_t *payload;
        next_response(&offset, &payload); next_response(&offset, &payload);
        airdap_frame_header_t h = next_response(&offset, &payload);
        assert(h.type == AIRDAP_FRAME_TYPE_CONTROL_RESPONSE && h.sequence == 3);
        const uint8_t accepted[] = {0x13, 0, 1};
        assert(h.payload_length == 3 && memcmp(payload, accepted, 3) == 0);
        h = next_response(&offset, &payload);
        assert_error_payload(payload, h.payload_length, AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE);
        h = next_response(&offset, &payload);
        const uint8_t rx[] = {0x14, 0, 0, 0, 0, 0xA5};
        assert(h.type == AIRDAP_FRAME_TYPE_CONTROL_RESPONSE && h.payload_length == 6);
        assert(memcmp(payload, rx, 6) == 0);
    }
    reset_connection_fakes();
    append_request(AIRDAP_FRAME_TYPE_HELLO, 9, 1, NULL, 0);
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, 2, write, 2);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 9, 3, NULL, 0);
    airdap_network_uart_handle_socket(TEST_CLIENT_FD);
    assert(uart_open_calls == 0 && uart_write_calls == 0);
    reset_connection_fakes(); uart_handshake(); fail_validation_at = 3;
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, 3, write, 2);
    airdap_network_uart_handle_socket(TEST_CLIENT_FD);
    assert(uart_open_calls == 1 && uart_close_calls == 1 && uart_write_calls == 0);
    reset_connection_fakes(); uart_handshake(); revoke_session_id_at_end = 77;
    airdap_network_uart_handle_socket(TEST_CLIENT_FD);
    assert(shutdown_calls == 1 && uart_close_calls == 2 && !uart_live);
    reset_connection_fakes(); uart_handshake();
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, 3, read, 3);
    block_write_after = 2 * AIRDAP_FRAME_HEADER_SIZE +
        AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE + strlen(identity.firmware_version) +
        AIRDAP_NETWORK_DAP_AUTH_RESPONSE_SIZE;
    airdap_network_uart_handle_socket(TEST_CLIENT_FD);
    assert(uart_close_calls == 1 && now_us >= 5000000 && !uart_live);
    assert_network_dap_status(true, 0, 0, 0, 0);
}

static void test_control_before_auth_is_rejected(void)
{
    reset_connection_fakes();
    const uint8_t request[] = {0x20U, 1U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 21U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 21U, 2U,
        request, sizeof(request));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert(error.type == AIRDAP_FRAME_TYPE_ERROR);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(dap_submit_calls == 0U);
    assert(board_calls == 0U);
}

static void test_control_wire_golden_and_replay(void)
{
    reset_connection_fakes();
    tls_read_chunk = 1U;
    const uint8_t request[] = {0x20U, 1U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 21U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 21U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 21U, 3U,
        request, sizeof(request));
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 21U, 3U,
        request, sizeof(request));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);
    assert(board_calls == 1U && board_opcode == 0x20U && board_value);
    assert(!control_operation_active);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    (void) next_response(&offset, &payload);
    static const uint8_t golden[] = {
        0x41, 0x44, 0x41, 0x50, 1, 6, 0, 0,
        0, 0, 0, 21, 0, 0, 0, 3, 0, 2, 0, 0, 0x20, 1,
    };
    assert(memcmp(tls_output + offset, golden, sizeof(golden)) == 0);
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE);
    assert(offset == tls_output_size);
}

static void test_control_errors_and_power_responses(void)
{
    const uint8_t operations[][2] = {{0x21, 1}, {0x21, 0}, {0x22, 0}};
    for (size_t index = 0U; index < 3U; ++index) {
        reset_connection_fakes();
        append_request(AIRDAP_FRAME_TYPE_HELLO, 22U, 1U, NULL, 0U);
        append_request(AIRDAP_FRAME_TYPE_AUTH, 22U, 2U, NULL, 0U);
        append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 22U, 3U,
            operations[index], index == 2U ? 1U : 2U);
        airdap_network_dap_handle_socket(TEST_CLIENT_FD);
        assert(board_calls == 1U && !control_operation_active);
        size_t offset = 0U;
        const uint8_t *payload = NULL;
        (void) next_response(&offset, &payload);
        (void) next_response(&offset, &payload);
        const airdap_frame_header_t response = next_response(&offset, &payload);
        assert(response.type == AIRDAP_FRAME_TYPE_CONTROL_RESPONSE);
        assert(response.session_id == 22U && response.sequence == 3U);
        assert(response.payload_length == 2U);
        assert(memcmp(payload, operations[index], 2U) == 0);
    }
    const airdap_frame_error_code_t errors[] = {
        AIRDAP_FRAME_ERROR_INVALID_ARGUMENT, AIRDAP_FRAME_ERROR_INTERNAL,
        AIRDAP_FRAME_ERROR_BUSY, AIRDAP_FRAME_ERROR_UNAUTHENTICATED,
    };
    for (size_t index = 0U; index < 4U; ++index) {
        reset_connection_fakes();
        uint8_t request[] = {0x21U, index == 0U ? 2U : 1U};
        if (index == 1U) { board_result = ESP_FAIL; }
        if (index == 2U) { admission_result = AIRDAP_MODE_DAP_BUSY; }
        if (index == 3U) { revoke_before_control_io = true; }
        append_request(AIRDAP_FRAME_TYPE_HELLO, 23U, 1U, NULL, 0U);
        append_request(AIRDAP_FRAME_TYPE_AUTH, 23U, 2U, NULL, 0U);
        append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 23U, 3U,
            request, sizeof(request));
        airdap_network_dap_handle_socket(TEST_CLIENT_FD);
        assert(board_calls == (index == 1U ? 1U : 0U));
        assert(!control_operation_active);
        size_t offset = 0U;
        const uint8_t *payload = NULL;
        (void) next_response(&offset, &payload);
        (void) next_response(&offset, &payload);
        const airdap_frame_header_t error = next_response(&offset, &payload);
        assert(error.type == AIRDAP_FRAME_TYPE_ERROR);
        assert(error.session_id == 23U && error.sequence == 3U);
        assert_error_payload(payload, error.payload_length, errors[index]);
        assert(payload[0] == 0U && payload[1] == errors[index]);
    }
}

static void test_control_revoked_before_output_suppresses_success(void)
{
    reset_connection_fakes();
    revoke_after_control_io = true;
    const uint8_t request[] = {0x20U, 0U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 24U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 24U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 24U, 3U,
        request, sizeof(request));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);
    assert(board_calls == 1U && !control_operation_active);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    (void) next_response(&offset, &payload);
    assert(offset == tls_output_size);
    assert(auth_close_calls == 1U && socket_close_calls == 1U);
}

static void test_control_opcodes_are_scoped_to_their_port(void)
{
    for (unsigned uart = 0U; uart <= 1U; ++uart) {
        const uint8_t first = uart ? 0x20U : 0x10U;
        const uint8_t last = uart ? 0x22U : 0x14U;
        for (uint8_t opcode = first; opcode <= last; ++opcode) {
            reset_connection_fakes();
            uart_handshake();
            append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9U, 3U,
                &opcode, 1U);
            if (uart) {
                airdap_network_uart_handle_socket(TEST_CLIENT_FD);
            } else {
                airdap_network_dap_handle_socket(TEST_CLIENT_FD);
            }
            assert(board_calls == 0U && uart_write_calls == 0U);
            assert(dap_submit_calls == 0U && !control_operation_active);
            assert(uart_open_calls == uart && uart_close_calls == uart);
            size_t offset = 0U;
            const uint8_t *payload = NULL;
            (void) next_response(&offset, &payload);
            (void) next_response(&offset, &payload);
            const airdap_frame_header_t response = next_response(&offset, &payload);
            assert(response.type == AIRDAP_FRAME_TYPE_ERROR);
            assert(response.session_id == 9U && response.sequence == 3U);
            assert_error_payload(payload, response.payload_length,
                AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
            assert(offset == tls_output_size);
        }
    }
}

static void test_ota_routes_auth_port_disconnect_and_reboot_ack(void)
{
    for (uint8_t opcode = 0x30; opcode <= 0x35; ++opcode) {
        for (unsigned lane = 0; lane < 3; ++lane) {
            reset_connection_fakes();
            append_request(AIRDAP_FRAME_TYPE_HELLO, 9, 1, NULL, 0);
            if (lane != 0) append_request(AIRDAP_FRAME_TYPE_AUTH, 9, 2, NULL, 0);
            append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, lane ? 3 : 2, &opcode, 1);
            if (lane == 2) airdap_network_uart_handle_socket(TEST_CLIENT_FD);
            else airdap_network_dap_handle_socket(TEST_CLIENT_FD);
            assert(ota_dispatch_calls == (lane == 1 ? 1U : 0U));
            assert(ota_disconnect_calls == (lane != 0 ? 1U : 0U));
            assert(ota_reboot_calls == (lane == 1 && opcode == 0x35 ? 1U : 0U));
            size_t offset = 0; const uint8_t *payload;
            (void) next_response(&offset, &payload);
            if (lane) (void) next_response(&offset, &payload);
            airdap_frame_header_t response = next_response(&offset, &payload);
            if (lane == 1) {
                assert(response.type == AIRDAP_FRAME_TYPE_CONTROL_RESPONSE && payload[0] == opcode);
            } else assert_error_payload(payload, response.payload_length,
                lane == 0 ? AIRDAP_FRAME_ERROR_UNAUTHENTICATED : AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
        }
    }
}

static void test_failed_ota_reboot_ack_does_not_restart(void)
{
    reset_connection_fakes();
    uart_handshake();
    const uint8_t reboot[] = {0x35};
    append_request(AIRDAP_FRAME_TYPE_CONTROL_REQUEST, 9, 3, reboot, sizeof(reboot));
    block_write_after = 2 * AIRDAP_FRAME_HEADER_SIZE +
        AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE + strlen(identity.firmware_version) +
        AIRDAP_NETWORK_DAP_AUTH_RESPONSE_SIZE;
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);
    assert(ota_dispatch_calls == 1 && ota_reboot_calls == 0);
    assert(ota_disconnect_calls == 1 && auth_close_calls == 1 && now_us >= 5000000);
}

int main(int argument_count, char **arguments)
{
    if (argument_count == 2 &&
        strcmp(arguments[1], "--uninitialized-status") == 0) {
        test_uninitialized_status_is_empty();
        puts("Uninitialized network DAP status test passed");
        return 0;
    }
    assert(argument_count == 1);
    reset_connection_fakes();
    test_status_is_empty_during_listener_startup();
    assert(listener_bind_calls == 2);
    test_concurrent_status_tracks_connection_lifecycle();
    test_authenticated_hello_dap_info_and_keepalive();
    test_dap_before_auth_is_rejected_without_dispatch();
    test_non_hello_first_frame_is_rejected_and_closed();
    test_oversized_dap_payload_returns_stable_error();
    test_usb_owner_returns_busy_without_dap_dispatch();
    test_invalid_auth_payload_never_marks_connection_authenticated();
    test_usb_ota_vendor_commands_are_not_exposed_over_dap_tcp();
    test_dap_timeout_discards_late_callback();
    test_sequence_error_is_stable_and_not_dispatched();
    test_malformed_header_is_closed_without_trusting_response_fields();
    test_revoke_shuts_down_registered_connection();
    test_unrelated_revoke_does_not_shutdown_registered_connection();
    test_revoke_closes_service_before_queued_dap_can_respond();
    test_pre_auth_partial_frame_times_out();
    test_uart_transport();
    test_control_before_auth_is_rejected();
    test_control_wire_golden_and_replay();
    test_control_errors_and_power_responses();
    test_control_revoked_before_output_suppresses_success();
    test_control_opcodes_are_scoped_to_their_port();
    test_ota_routes_auth_port_disconnect_and_reboot_ack();
    test_failed_ota_reboot_ack_does_not_restart();
    assert_network_dap_status(true, 0U, 0U, 0U, 0U);
    puts("Network DAP transport tests passed");
    return 0;
}
