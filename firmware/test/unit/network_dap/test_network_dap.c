#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airdap_dap_service.h"
#include "airdap_device_identity.h"
#include "airdap_frame.h"
#include "airdap_mode_state.h"
#include "airdap_network_auth.h"
#include "airdap_network_dap.h"
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
    bool locked;
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
static unsigned int queue_timeout_count;
static airdap_network_auth_revoke_fn revoke_handler;
static void *revoke_context;
static airdap_dap_response_fn saved_response_callback;
static void *saved_response_context;
static airdap_dap_response_token_t saved_response_token;
static bool callback_delivered_during_close;

static void reset_connection_fakes(void)
{
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
    assert(tls_output_size + length <= sizeof(tls_output));
    memcpy(tls_output + tls_output_size, buffer, length);
    tls_output_size += length;
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
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore != NULL && timeout == portMAX_DELAY);
    assert(!semaphore->locked);
    semaphore->locked = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL && semaphore->locked);
    semaphore->locked = false;
    return pdTRUE;
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
    if (timeout_at_end_of_input && descriptors[0].fd == TEST_CLIENT_FD) {
        now_us += (int64_t) timeout_ms * 1000;
        return 0;
    }
    descriptors[0].revents = descriptors[0].events;
    return 1;
}

static void start_component_once(void)
{
    assert(airdap_network_dap_start() == ESP_OK);
    assert(task_create_calls == 1U);
    assert(revoke_handler != NULL);
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

static void test_usb_owner_returns_busy_without_dap_dispatch(void)
{
    reset_connection_fakes();
    admission_result = AIRDAP_MODE_DAP_BUSY;
    static const uint8_t dap_connect[] = {0x02U, 0x01U};
    append_request(AIRDAP_FRAME_TYPE_HELLO, 11U, 1U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_AUTH, 11U, 2U, NULL, 0U);
    append_request(AIRDAP_FRAME_TYPE_DAP_REQUEST, 11U, 3U,
        dap_connect, sizeof(dap_connect));
    airdap_network_dap_handle_socket(TEST_CLIENT_FD);

    assert(dap_submit_calls == 0U);
    size_t offset = 0U;
    const uint8_t *payload = NULL;
    (void) next_response(&offset, &payload);
    (void) next_response(&offset, &payload);
    const airdap_frame_header_t error = next_response(&offset, &payload);
    assert_error_payload(payload, error.payload_length,
        AIRDAP_FRAME_ERROR_BUSY);
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

int main(void)
{
    reset_connection_fakes();
    start_component_once();
    test_authenticated_hello_dap_info_and_keepalive();
    test_dap_before_auth_is_rejected_without_dispatch();
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
    puts("Network DAP transport tests passed");
    return 0;
}
