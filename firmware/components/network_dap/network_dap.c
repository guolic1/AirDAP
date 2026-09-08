#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "airdap_dap_service.h"
#include "airdap_device_identity.h"
#include "airdap_frame.h"
#include "airdap_mode_state.h"
#include "airdap_network_auth.h"
#include "airdap_network_dap.h"
#include "airdap_network_dap_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    LISTENER_TASK_STACK_SIZE = 4096,
    LISTENER_TASK_PRIORITY = 5,
    CONNECTION_TASK_STACK_SIZE = 8192,
    CONNECTION_TASK_PRIORITY = 5,
    LISTENER_POLL_MS = 100,
    APPLICATION_IO_TIMEOUT_US = 5000000,
    REVOKE_QUEUE_DEPTH = 4,
    FRAME_BUFFER_SIZE = AIRDAP_FRAME_HEADER_SIZE +
        AIRDAP_FRAME_MAX_PAYLOAD_SIZE,
    RESPONSE_BUFFER_SIZE = AIRDAP_FRAME_HEADER_SIZE +
        AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE,
    DAP_CONNECT_COMMAND = 0x02,
    AIRDAP_USB_OTA_FIRST_COMMAND = 0x80,
    AIRDAP_USB_OTA_LAST_COMMAND = 0x85,
};

_Static_assert(
    AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE ==
        AIRDAP_DEVICE_UUID_SIZE + sizeof(uint32_t) +
            AIRDAP_DEVICE_SERIAL_LENGTH,
    "HELLO fixed payload size must match the identity fields");
_Static_assert(
    AIRDAP_NETWORK_DAP_AUTH_RESPONSE_SIZE ==
        sizeof(uint32_t) + AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE,
    "AUTH response size must match the session fields");

typedef struct {
    airdap_dap_response_token_t token;
    size_t response_size;
    uint8_t response[AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE];
} dap_response_item_t;

typedef struct {
    bool allocated;
    bool registered;
    bool dap_claimed;
    int socket_fd;
    airdap_network_auth_connection_t *auth_connection;
    QueueHandle_t response_queue;
    atomic_uint auth_session_id;
    atomic_uint dap_session;
    atomic_uintptr_t pending_response_token;
    uintptr_t next_response_token;
    uint8_t frame_buffer[FRAME_BUFFER_SIZE];
    uint8_t response_buffer[RESPONSE_BUFFER_SIZE];
} network_connection_t;

typedef enum {
    IO_OK = 0,
    IO_CLOSED,
    IO_TIMEOUT,
    IO_FAILED,
} io_result_t;

typedef enum {
    FRAME_READ_OK = 0,
    FRAME_READ_CLOSED,
    FRAME_READ_TIMEOUT,
    FRAME_READ_INVALID,
    FRAME_READ_FAILED,
} frame_read_result_t;

static const char *TAG = "airdap_net_dap";
static SemaphoreHandle_t registry_mutex;
static QueueHandle_t revoke_queue;
static network_connection_t connections[AIRDAP_NETWORK_DAP_MAX_CONNECTIONS];
static const airdap_device_identity_t *device_identity;
static atomic_bool initialization_started;
static atomic_bool revoke_all;
static bool dap_claimed;
static int listener_socket = -1;

static void clear_bytes(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static bool registry_lock(void)
{
    return registry_mutex != NULL &&
        xSemaphoreTake(registry_mutex, portMAX_DELAY) == pdTRUE;
}

static void registry_unlock(void)
{
    (void) xSemaphoreGive(registry_mutex);
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t) (value >> 24U);
    output[1] = (uint8_t) (value >> 16U);
    output[2] = (uint8_t) (value >> 8U);
    output[3] = (uint8_t) value;
}

static bool make_nonblocking(int socket_fd)
{
    const int flags = fcntl(socket_fd, F_GETFL);
    return flags >= 0 &&
        ((flags & O_NONBLOCK) != 0 ||
            fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static int poll_timeout_ms(int64_t deadline_us)
{
    if (deadline_us == INT64_MAX) {
        return LISTENER_POLL_MS;
    }
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    const int64_t rounded_ms = (remaining_us + 999) / 1000;
    return rounded_ms > INT_MAX ? INT_MAX : (int) rounded_ms;
}

static io_result_t wait_for_socket(
    int socket_fd,
    short events,
    int64_t deadline_us)
{
    for (;;) {
        const int timeout_ms = poll_timeout_ms(deadline_us);
        if (deadline_us != INT64_MAX && timeout_ms == 0) {
            return IO_TIMEOUT;
        }
        struct pollfd descriptor = {
            .fd = socket_fd,
            .events = events,
        };
        const int ready = poll(&descriptor, 1U, timeout_ms);
        if (ready == 0) {
            if (deadline_us == INT64_MAX) {
                continue;
            }
            return IO_TIMEOUT;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return IO_FAILED;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return IO_CLOSED;
        }
        if ((descriptor.revents & events) != 0) {
            return IO_OK;
        }
    }
}

static io_result_t tls_read_exact(
    network_connection_t *connection,
    uint8_t *output,
    size_t output_size,
    int64_t deadline_us)
{
    size_t offset = 0U;
    while (offset < output_size) {
        const ssize_t received = airdap_network_auth_tls_read(
            connection->auth_connection,
            output + offset,
            output_size - offset);
        if (received > 0) {
            offset += (size_t) received;
            continue;
        }
        if (received == 0) {
            return IO_CLOSED;
        }
        short events;
        if (received == ESP_TLS_ERR_SSL_WANT_READ) {
            events = POLLIN;
        } else if (received == ESP_TLS_ERR_SSL_WANT_WRITE) {
            events = POLLOUT;
        } else {
            return IO_FAILED;
        }
        const io_result_t wait_result = wait_for_socket(
            connection->socket_fd,
            events,
            deadline_us);
        if (wait_result != IO_OK) {
            return wait_result;
        }
    }
    return IO_OK;
}

static io_result_t tls_write_all(
    network_connection_t *connection,
    const uint8_t *input,
    size_t input_size,
    int64_t deadline_us)
{
    size_t offset = 0U;
    while (offset < input_size) {
        const ssize_t written = airdap_network_auth_tls_write(
            connection->auth_connection,
            input + offset,
            input_size - offset);
        if (written > 0) {
            offset += (size_t) written;
            continue;
        }
        if (written == 0) {
            return IO_CLOSED;
        }
        short events;
        if (written == ESP_TLS_ERR_SSL_WANT_WRITE) {
            events = POLLOUT;
        } else if (written == ESP_TLS_ERR_SSL_WANT_READ) {
            events = POLLIN;
        } else {
            return IO_FAILED;
        }
        const io_result_t wait_result = wait_for_socket(
            connection->socket_fd,
            events,
            deadline_us);
        if (wait_result != IO_OK) {
            return wait_result;
        }
    }
    return IO_OK;
}

static frame_read_result_t read_frame(
    network_connection_t *connection,
    int64_t deadline_us,
    airdap_frame_header_t *header,
    const uint8_t **payload)
{
    const io_result_t header_result = tls_read_exact(
        connection,
        connection->frame_buffer,
        AIRDAP_FRAME_HEADER_SIZE,
        deadline_us);
    if (header_result != IO_OK) {
        return header_result == IO_CLOSED ? FRAME_READ_CLOSED :
            header_result == IO_TIMEOUT ? FRAME_READ_TIMEOUT :
            FRAME_READ_FAILED;
    }

    size_t frame_size = 0U;
    airdap_frame_error_code_t error_code = AIRDAP_FRAME_ERROR_INTERNAL;
    airdap_frame_decode_status_t decode_status = airdap_frame_decode(
        connection->frame_buffer,
        AIRDAP_FRAME_HEADER_SIZE,
        header,
        payload,
        &frame_size,
        &error_code);
    if (decode_status == AIRDAP_FRAME_DECODE_INVALID_FRAME) {
        return FRAME_READ_INVALID;
    }
    if (decode_status == AIRDAP_FRAME_DECODE_OK) {
        return FRAME_READ_OK;
    }
    if (error_code != AIRDAP_FRAME_ERROR_TRUNCATED) {
        return FRAME_READ_INVALID;
    }

    const size_t payload_size =
        ((size_t) connection->frame_buffer[16] << 8U) |
        (size_t) connection->frame_buffer[17];
    if (payload_size == 0U ||
        payload_size > AIRDAP_FRAME_MAX_PAYLOAD_SIZE) {
        return FRAME_READ_INVALID;
    }
    const io_result_t payload_result = tls_read_exact(
        connection,
        connection->frame_buffer + AIRDAP_FRAME_HEADER_SIZE,
        payload_size,
        deadline_us);
    if (payload_result != IO_OK) {
        return payload_result == IO_CLOSED ? FRAME_READ_CLOSED :
            payload_result == IO_TIMEOUT ? FRAME_READ_TIMEOUT :
            FRAME_READ_FAILED;
    }

    decode_status = airdap_frame_decode(
        connection->frame_buffer,
        AIRDAP_FRAME_HEADER_SIZE + payload_size,
        header,
        payload,
        &frame_size,
        &error_code);
    return decode_status == AIRDAP_FRAME_DECODE_OK
        ? FRAME_READ_OK
        : FRAME_READ_INVALID;
}

static bool send_frame(
    network_connection_t *connection,
    const airdap_frame_header_t *request,
    uint8_t response_type,
    const uint8_t *payload,
    size_t payload_size)
{
    if (payload_size > UINT16_MAX) {
        return false;
    }
    airdap_frame_header_t response = *request;
    response.type = response_type;
    response.payload_length = (uint16_t) payload_size;
    size_t encoded_size = 0U;
    if (airdap_frame_encode(
            &response,
            payload,
            connection->response_buffer,
            sizeof(connection->response_buffer),
            &encoded_size) != AIRDAP_FRAME_ERROR_NONE) {
        return false;
    }
    return tls_write_all(
        connection,
        connection->response_buffer,
        encoded_size,
        esp_timer_get_time() + APPLICATION_IO_TIMEOUT_US) == IO_OK;
}

static bool send_error(
    network_connection_t *connection,
    const airdap_frame_header_t *request,
    airdap_frame_error_code_t error_code)
{
    uint8_t payload[AIRDAP_FRAME_ERROR_CODE_SIZE];
    return airdap_frame_error_code_encode(
            error_code,
            payload,
            sizeof(payload)) &&
        send_frame(
            connection,
            request,
            AIRDAP_FRAME_TYPE_ERROR,
            payload,
            sizeof(payload));
}

static network_connection_t *allocate_connection(int socket_fd)
{
    QueueHandle_t response_queue = xQueueCreate(1U, sizeof(dap_response_item_t));
    if (response_queue == NULL || !registry_lock()) {
        if (response_queue != NULL) {
            vQueueDelete(response_queue);
        }
        return NULL;
    }

    network_connection_t *selected = NULL;
    for (size_t index = 0U;
         index < AIRDAP_NETWORK_DAP_MAX_CONNECTIONS;
         ++index) {
        if (!connections[index].allocated) {
            selected = &connections[index];
            selected->allocated = true;
            selected->registered = true;
            selected->dap_claimed = false;
            selected->socket_fd = socket_fd;
            selected->auth_connection = NULL;
            selected->response_queue = response_queue;
            selected->next_response_token = 1U;
            atomic_store(&selected->auth_session_id, 0U);
            atomic_store(&selected->dap_session, 0U);
            atomic_store(&selected->pending_response_token, 0U);
            break;
        }
    }
    registry_unlock();
    if (selected == NULL) {
        vQueueDelete(response_queue);
    }
    return selected;
}

static bool claim_dap_connection(network_connection_t *connection)
{
    if (!registry_lock()) {
        return false;
    }
    const bool available = !dap_claimed;
    if (available) {
        dap_claimed = true;
        connection->dap_claimed = true;
    }
    registry_unlock();
    return available;
}

static void unregister_connection(network_connection_t *connection)
{
    if (!registry_lock()) {
        return;
    }
    connection->registered = false;
    atomic_store(&connection->auth_session_id, 0U);
    registry_unlock();
}

static void release_connection(network_connection_t *connection)
{
    unregister_connection(connection);
    atomic_store(&connection->pending_response_token, 0U);

    const airdap_dap_session_id_t dap_session = atomic_exchange(
        &connection->dap_session,
        0U);
    if (dap_session != 0U) {
        const airdap_dap_service_result_t result =
            airdap_dap_service_session_close(
                AIRDAP_DAP_TRANSPORT_NETWORK,
                dap_session);
        if (result != AIRDAP_DAP_SERVICE_OK &&
            result != AIRDAP_DAP_SERVICE_STALE_SESSION) {
            ESP_LOGW(TAG, "Unable to close network DAP session: %u",
                (unsigned int) result);
        }
    }

    if (connection->dap_claimed && registry_lock()) {
        dap_claimed = false;
        connection->dap_claimed = false;
        registry_unlock();
    }
    if (connection->auth_connection != NULL) {
        airdap_network_auth_connection_close(connection->auth_connection);
        connection->auth_connection = NULL;
    }
    clear_bytes(connection->frame_buffer, sizeof(connection->frame_buffer));
    clear_bytes(
        connection->response_buffer,
        sizeof(connection->response_buffer));
    if (connection->response_queue != NULL) {
        vQueueDelete(connection->response_queue);
        connection->response_queue = NULL;
    }
    (void) close(connection->socket_fd);

    if (registry_lock()) {
        connection->socket_fd = -1;
        connection->allocated = false;
        registry_unlock();
    }
}

static void revoke_session(void *context, uint32_t session_id)
{
    (void) context;
    if (session_id == 0U) {
        return;
    }
    if (revoke_queue == NULL ||
        xQueueSend(revoke_queue, &session_id, 0U) != pdTRUE) {
        /* Losing a revocation is unsafe. A saturated queue therefore asks the
         * listener to close every authenticated connection on its next pass. */
        atomic_store(&revoke_all, true);
    }
}

static void process_revoked_session(uint32_t session_id, bool all_sessions)
{
    airdap_dap_session_id_t dap_sessions[
        AIRDAP_NETWORK_DAP_MAX_CONNECTIONS] = {0};
    size_t dap_session_count = 0U;
    if (!registry_lock()) {
        atomic_store(&revoke_all, true);
        return;
    }
    for (size_t index = 0U;
         index < AIRDAP_NETWORK_DAP_MAX_CONNECTIONS;
         ++index) {
        network_connection_t *connection = &connections[index];
        const uint32_t current_session =
            atomic_load(&connection->auth_session_id);
        if (connection->allocated && connection->registered &&
            current_session != 0U &&
            (all_sessions || current_session == session_id)) {
            (void) shutdown(connection->socket_fd, SHUT_RDWR);
            const airdap_dap_session_id_t dap_session =
                atomic_load(&connection->dap_session);
            if (dap_session != 0U) {
                dap_sessions[dap_session_count++] = dap_session;
            }
        }
    }
    registry_unlock();

    /* Do not clear dap_session here. The connection task remains the teardown
     * owner and must also pass through session_close before deleting its
     * response queue. Concurrent closes serialize on dap_service's mutex. */
    for (size_t index = 0U; index < dap_session_count; ++index) {
        (void) airdap_dap_service_session_close(
            AIRDAP_DAP_TRANSPORT_NETWORK,
            dap_sessions[index]);
    }
}

void airdap_network_dap_process_revocations(void)
{
    if (atomic_exchange(&revoke_all, false)) {
        process_revoked_session(0U, true);
    }
    uint32_t session_id = 0U;
    while (revoke_queue != NULL &&
           xQueueReceive(revoke_queue, &session_id, 0U) == pdTRUE) {
        process_revoked_session(session_id, false);
    }
}

static bool queue_dap_response(
    void *context,
    airdap_dap_transport_t transport,
    airdap_dap_session_id_t session,
    airdap_dap_response_token_t token,
    const uint8_t *response,
    size_t response_length)
{
    network_connection_t *connection = context;
    if (connection == NULL || transport != AIRDAP_DAP_TRANSPORT_NETWORK ||
        session != atomic_load(&connection->dap_session) || token == 0U ||
        token != atomic_load(&connection->pending_response_token) ||
        response == NULL || response_length == 0U ||
        response_length > AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE) {
        return false;
    }
    dap_response_item_t item = {
        .token = token,
        .response_size = response_length,
    };
    memcpy(item.response, response, response_length);
    return xQueueSend(connection->response_queue, &item, 0U) == pdTRUE;
}

static airdap_frame_error_code_t auth_error_code(
    airdap_network_auth_result_t result)
{
    switch (result) {
    case AIRDAP_NETWORK_AUTH_BUSY:
        return AIRDAP_FRAME_ERROR_BUSY;
    case AIRDAP_NETWORK_AUTH_NO_CREDENTIAL:
    case AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED:
    case AIRDAP_NETWORK_AUTH_UNAUTHENTICATED:
    case AIRDAP_NETWORK_AUTH_EXPIRED:
    case AIRDAP_NETWORK_AUTH_REPLAY:
        return AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    default:
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
}

static airdap_frame_error_code_t dap_service_error_code(
    airdap_dap_service_result_t result)
{
    switch (result) {
    case AIRDAP_DAP_SERVICE_BUSY:
    case AIRDAP_DAP_SERVICE_QUEUE_FULL:
        return AIRDAP_FRAME_ERROR_BUSY;
    case AIRDAP_DAP_SERVICE_STALE_SESSION:
    case AIRDAP_DAP_SERVICE_UNAUTHENTICATED:
        return AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    default:
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
}

static bool send_hello(
    network_connection_t *connection,
    const airdap_frame_header_t *request)
{
    const size_t firmware_version_size = strlen(
        device_identity->firmware_version);
    uint8_t payload[AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE + 31U];
    if (firmware_version_size == 0U || firmware_version_size > 31U) {
        return send_error(connection, request, AIRDAP_FRAME_ERROR_INTERNAL);
    }
    memcpy(payload, device_identity->uuid, AIRDAP_DEVICE_UUID_SIZE);
    write_u32_be(payload + AIRDAP_DEVICE_UUID_SIZE,
        device_identity->capabilities);
    memcpy(
        payload + AIRDAP_DEVICE_UUID_SIZE + sizeof(uint32_t),
        device_identity->device_id,
        AIRDAP_DEVICE_SERIAL_LENGTH);
    memcpy(
        payload + AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE,
        device_identity->firmware_version,
        firmware_version_size);
    return send_frame(
        connection,
        request,
        AIRDAP_FRAME_TYPE_HELLO,
        payload,
        AIRDAP_NETWORK_DAP_HELLO_FIXED_SIZE + firmware_version_size);
}

static bool bind_session(
    network_connection_t *connection,
    const airdap_frame_header_t *request,
    const uint8_t *payload)
{
    if (request->payload_length != 0U &&
        request->payload_length != AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE) {
        (void) send_error(
            connection,
            request,
            AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
        return false;
    }
    if (!claim_dap_connection(connection)) {
        (void) send_error(connection, request, AIRDAP_FRAME_ERROR_BUSY);
        return false;
    }

    airdap_network_auth_session_info_t session_info = {0};
    const airdap_network_auth_result_t bind_result =
        airdap_network_auth_session_bind(
            connection->auth_connection,
            request->payload_length == 0U ? NULL : payload,
            request->payload_length,
            &session_info);
    if (bind_result != AIRDAP_NETWORK_AUTH_OK) {
        (void) send_error(
            connection,
            request,
            auth_error_code(bind_result));
        return false;
    }
    atomic_store(&connection->auth_session_id, session_info.session_id);
    const airdap_network_auth_result_t validation_result =
        airdap_network_auth_session_validate(
            connection->auth_connection,
            session_info.session_id);
    if (validation_result != AIRDAP_NETWORK_AUTH_OK) {
        (void) send_error(
            connection,
            request,
            auth_error_code(validation_result));
        clear_bytes(&session_info, sizeof(session_info));
        return false;
    }

    airdap_dap_session_id_t dap_session = 0U;
    const airdap_dap_service_result_t open_result =
        airdap_dap_service_session_open(
            AIRDAP_DAP_TRANSPORT_NETWORK,
            true,
            &dap_session);
    if (open_result != AIRDAP_DAP_SERVICE_OK) {
        (void) send_error(
            connection,
            request,
            dap_service_error_code(open_result));
        clear_bytes(&session_info, sizeof(session_info));
        return false;
    }
    atomic_store(&connection->dap_session, dap_session);

    uint8_t response[AIRDAP_NETWORK_DAP_AUTH_RESPONSE_SIZE];
    write_u32_be(response, session_info.session_id);
    memcpy(
        response + sizeof(uint32_t),
        session_info.session_token,
        AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE);
    const bool sent = send_frame(
        connection,
        request,
        AIRDAP_FRAME_TYPE_AUTH,
        response,
        sizeof(response));
    clear_bytes(response, sizeof(response));
    clear_bytes(&session_info, sizeof(session_info));
    return sent;
}

static bool validate_bound_session(network_connection_t *connection)
{
    const uint32_t session_id = atomic_load(&connection->auth_session_id);
    return session_id != 0U &&
        airdap_network_auth_session_validate(
            connection->auth_connection,
            session_id) == AIRDAP_NETWORK_AUTH_OK;
}

static bool send_dap_response(
    network_connection_t *connection,
    const airdap_frame_header_t *request,
    const uint8_t *payload)
{
    if (request->payload_length == 0U) {
        return send_error(connection, request, AIRDAP_FRAME_ERROR_TRUNCATED);
    }
    if (payload[0] >= AIRDAP_USB_OTA_FIRST_COMMAND &&
        payload[0] <= AIRDAP_USB_OTA_LAST_COMMAND) {
        return send_error(
            connection,
            request,
            AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
    }
    if (!validate_bound_session(connection)) {
        (void) send_error(
            connection,
            request,
            AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
        return false;
    }
    if (payload[0] == DAP_CONNECT_COMMAND) {
        const airdap_mode_dap_result_t admission =
            airdap_mode_state_dap_admission(
                AIRDAP_DAP_OWNER_NETWORK,
                true);
        if (admission != AIRDAP_MODE_DAP_ALLOWED) {
            const airdap_frame_error_code_t error_code =
                admission == AIRDAP_MODE_DAP_UNAUTHENTICATED
                ? AIRDAP_FRAME_ERROR_UNAUTHENTICATED
                : admission == AIRDAP_MODE_DAP_BUSY ||
                    admission == AIRDAP_MODE_DAP_OFFLINE
                ? AIRDAP_FRAME_ERROR_BUSY
                : AIRDAP_FRAME_ERROR_INTERNAL;
            return send_error(connection, request, error_code);
        }
    }

    airdap_dap_response_token_t token = connection->next_response_token++;
    if (token == 0U) {
        token = connection->next_response_token++;
    }
    atomic_store(&connection->pending_response_token, token);
    const airdap_dap_service_result_t submit_result =
        airdap_dap_service_submit(
            AIRDAP_DAP_TRANSPORT_NETWORK,
            atomic_load(&connection->dap_session),
            payload,
            request->payload_length,
            token,
            queue_dap_response,
            connection);
    if (submit_result != AIRDAP_DAP_SERVICE_OK) {
        atomic_store(&connection->pending_response_token, 0U);
        return send_error(
            connection,
            request,
            dap_service_error_code(submit_result));
    }

    dap_response_item_t response;
    for (;;) {
        if (xQueueReceive(
                connection->response_queue,
                &response,
                pdMS_TO_TICKS(
                    AIRDAP_DAP_SERVICE_REQUEST_TIMEOUT_US / 1000U)) !=
            pdTRUE) {
            atomic_store(&connection->pending_response_token, 0U);
            return send_error(
                connection,
                request,
                AIRDAP_FRAME_ERROR_TIMEOUT);
        }
        if (response.token == token) {
            break;
        }
    }
    atomic_store(&connection->pending_response_token, 0U);
    if (!validate_bound_session(connection)) {
        (void) send_error(
            connection,
            request,
            AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
        return false;
    }
    return send_frame(
        connection,
        request,
        AIRDAP_FRAME_TYPE_DAP_RESPONSE,
        response.response,
        response.response_size);
}

static void run_connection(network_connection_t *connection)
{
    if (!make_nonblocking(connection->socket_fd)) {
        return;
    }
    const airdap_network_auth_result_t tls_result =
        airdap_network_auth_tls_accept(
            connection->socket_fd,
            &connection->auth_connection);
    if (tls_result != AIRDAP_NETWORK_AUTH_OK) {
        return;
    }

    const int64_t pre_auth_deadline = esp_timer_get_time() +
        APPLICATION_IO_TIMEOUT_US;
    bool hello_complete = false;
    bool authenticated = false;
    bool validator_initialized = false;
    airdap_frame_sequence_validator_t validator = {0};

    for (;;) {
        airdap_frame_header_t request = {0};
        const uint8_t *payload = NULL;
        const frame_read_result_t read_result = read_frame(
            connection,
            authenticated ? INT64_MAX : pre_auth_deadline,
            &request,
            &payload);
        if (read_result != FRAME_READ_OK) {
            return;
        }
        if (!validator_initialized) {
            if (airdap_frame_sequence_validator_init(
                    &validator,
                    request.session_id) != AIRDAP_FRAME_ERROR_NONE) {
                return;
            }
            validator_initialized = true;
        }
        const airdap_frame_error_code_t sequence_error =
            airdap_frame_validate_request_sequence(&validator, &request);
        if (sequence_error != AIRDAP_FRAME_ERROR_NONE) {
            if (!send_error(connection, &request, sequence_error)) {
                return;
            }
            continue;
        }

        switch (request.type) {
        case AIRDAP_FRAME_TYPE_HELLO:
            if (request.payload_length != 0U) {
                if (!send_error(
                        connection,
                        &request,
                        AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE)) {
                    return;
                }
                break;
            }
            if (!send_hello(connection, &request)) {
                return;
            }
            hello_complete = true;
            break;

        case AIRDAP_FRAME_TYPE_AUTH:
            if (!hello_complete || authenticated) {
                if (!send_error(
                        connection,
                        &request,
                        AIRDAP_FRAME_ERROR_UNAUTHENTICATED)) {
                    return;
                }
                break;
            }
            if (!bind_session(connection, &request, payload)) {
                return;
            }
            authenticated = true;
            break;

        case AIRDAP_FRAME_TYPE_DAP_REQUEST:
            if (!authenticated) {
                if (!send_error(
                        connection,
                        &request,
                        AIRDAP_FRAME_ERROR_UNAUTHENTICATED)) {
                    return;
                }
                break;
            }
            if (!send_dap_response(connection, &request, payload)) {
                return;
            }
            break;

        case AIRDAP_FRAME_TYPE_KEEPALIVE:
            if (!authenticated || request.payload_length != 0U ||
                !validate_bound_session(connection)) {
                if (!send_error(
                        connection,
                        &request,
                        AIRDAP_FRAME_ERROR_UNAUTHENTICATED)) {
                    return;
                }
                break;
            }
            if (!send_frame(
                    connection,
                    &request,
                    AIRDAP_FRAME_TYPE_KEEPALIVE,
                    NULL,
                    0U)) {
                return;
            }
            break;

        default:
            if (!send_error(
                    connection,
                    &request,
                    AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE)) {
                return;
            }
            break;
        }
    }
}

void airdap_network_dap_handle_socket(int socket_fd)
{
    network_connection_t *connection = allocate_connection(socket_fd);
    if (connection == NULL) {
        (void) close(socket_fd);
        return;
    }
    run_connection(connection);
    release_connection(connection);
}

static void connection_task(void *argument)
{
    network_connection_t *connection = argument;
    run_connection(connection);
    release_connection(connection);
    vTaskDelete(NULL);
}

static void listener_task(void *argument)
{
    (void) argument;
    for (;;) {
        airdap_network_dap_process_revocations();
        struct pollfd descriptor = {
            .fd = listener_socket,
            .events = POLLIN,
        };
        const int ready = poll(&descriptor, 1U, LISTENER_POLL_MS);
        if (ready <= 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ESP_LOGE(TAG, "DAP TCP listener failed");
            continue;
        }
        if ((descriptor.revents & POLLIN) == 0) {
            continue;
        }

        const int accepted_socket = accept(listener_socket, NULL, NULL);
        if (accepted_socket < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGW(TAG, "DAP TCP accept failed: %d", errno);
            }
            continue;
        }
        network_connection_t *connection = allocate_connection(
            accepted_socket);
        if (connection == NULL) {
            (void) close(accepted_socket);
            continue;
        }
        if (xTaskCreate(
                connection_task,
                "dap_tcp_connection",
                CONNECTION_TASK_STACK_SIZE,
                connection,
                CONNECTION_TASK_PRIORITY,
                NULL) != pdPASS) {
            release_connection(connection);
        }
    }
}

esp_err_t airdap_network_dap_start(void)
{
    if (atomic_exchange(&initialization_started, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    device_identity = airdap_device_identity_get();
    if (device_identity == NULL || device_identity->firmware_version == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    registry_mutex = xSemaphoreCreateMutex();
    if (registry_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    revoke_queue = xQueueCreate(REVOKE_QUEUE_DEPTH, sizeof(uint32_t));
    if (revoke_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    listener_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_socket < 0) {
        return ESP_FAIL;
    }
    const int reuse_address = 1;
    if (setsockopt(
            listener_socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) != 0 ||
        !make_nonblocking(listener_socket)) {
        (void) close(listener_socket);
        listener_socket = -1;
        return ESP_FAIL;
    }
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(AIRDAP_NETWORK_DAP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(
            listener_socket,
            (const struct sockaddr *) &address,
            sizeof(address)) != 0 ||
        listen(listener_socket, AIRDAP_NETWORK_DAP_MAX_CONNECTIONS) != 0) {
        (void) close(listener_socket);
        listener_socket = -1;
        return ESP_FAIL;
    }
    const esp_err_t revoke_error = airdap_network_auth_set_revoke_handler(
        revoke_session,
        NULL);
    if (revoke_error != ESP_OK) {
        (void) close(listener_socket);
        listener_socket = -1;
        return revoke_error;
    }
    if (xTaskCreate(
            listener_task,
            "dap_tcp_listener",
            LISTENER_TASK_STACK_SIZE,
            NULL,
            LISTENER_TASK_PRIORITY,
            NULL) != pdPASS) {
        (void) close(listener_socket);
        listener_socket = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Authenticated DAP TCP listener started on port %u",
        AIRDAP_NETWORK_DAP_PORT);
    return ESP_OK;
}
