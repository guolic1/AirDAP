#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>

#include "sdkconfig.h"

#include "airdap_config_store.h"
#include "airdap_device_identity.h"
#include "airdap_network_auth.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "psa/crypto.h"

#if !CONFIG_ESP_TLS_PSK_VERIFICATION
#error "AirDAP network authentication requires ESP-TLS PSK verification"
#endif
#if !CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
#error "AirDAP network authentication requires TLS 1.3"
#endif
#if !CONFIG_MBEDTLS_SSL_TLS1_3_KEXM_PSK_EPHEMERAL
#error "AirDAP network authentication requires TLS 1.3 PSK-DHE"
#endif
#if CONFIG_MBEDTLS_SSL_TLS1_3_KEXM_PSK
#error "AirDAP network authentication forbids TLS 1.3 pure PSK"
#endif
#if CONFIG_MBEDTLS_SSL_TLS1_3_KEXM_EPHEMERAL
#error "AirDAP network authentication forbids unauthenticated ephemeral TLS"
#endif
#if CONFIG_MBEDTLS_SSL_EARLY_DATA
#error "AirDAP network authentication forbids TLS 1.3 early data"
#endif
#if CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS || \
    CONFIG_ESP_TLS_SERVER_SESSION_TICKETS
#error "AirDAP network authentication forbids server session tickets"
#endif

enum {
    CREDENTIAL_RECORD_SIZE = 44,
    CREDENTIAL_RECORD_VERSION = 1,
    CREDENTIAL_RECORD_GENERATION_OFFSET = 8,
    CREDENTIAL_RECORD_PSK_OFFSET = 12,
    PSK_IDENTITY_SIZE = 7 + 17,
};

typedef struct {
    bool valid;
    uint32_t generation;
    uint8_t psk[AIRDAP_NETWORK_AUTH_PSK_SIZE];
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
} active_credential_t;

typedef struct {
    bool active;
    uint32_t session_id;
    uint32_t credential_generation;
    uint8_t token[AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE];
    uint8_t credential_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    int64_t last_activity_us;
} owner_session_t;

typedef struct {
    airdap_network_auth_revoke_fn handler;
    void *context;
    uint32_t session_id;
} pending_revoke_t;

struct airdap_network_auth_connection {
    esp_tls_t *tls;
    psk_hint_key_t psk_hint_key;
    uint32_t credential_generation;
    uint8_t psk[AIRDAP_NETWORK_AUTH_PSK_SIZE];
    uint8_t credential_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    char psk_identity[PSK_IDENTITY_SIZE];
    bool tls_authenticated;
    bool bound;
    uint32_t session_id;
};

static const uint8_t credential_magic[4] = {'A', 'N', 'E', 'T'};
static const uint8_t fingerprint_label[] = "AirDAP network PSK v1";
static const int tls_ciphersuites[] = {
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
    0,
};

static SemaphoreHandle_t auth_mutex;
static esp_timer_handle_t expiry_timer;
static bool initialized;
static char active_identity[PSK_IDENTITY_SIZE];
static active_credential_t active_credential;
static owner_session_t owner_session;
static unsigned int pending_handshakes;
static uint32_t next_session_id = 1U;
static airdap_network_auth_revoke_fn revoke_handler;
static void *revoke_context;

static void clear_bytes(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static bool lock_auth(void)
{
    return auth_mutex != NULL &&
        xSemaphoreTake(auth_mutex, portMAX_DELAY) == pdTRUE;
}

static bool try_lock_auth(void)
{
    return auth_mutex != NULL && xSemaphoreTake(auth_mutex, 0U) == pdTRUE;
}

static void unlock_auth(void)
{
    (void) xSemaphoreGive(auth_mutex);
}

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t) (((uint16_t) input[0] << 8U) | input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t) input[0] << 24U) |
        ((uint32_t) input[1] << 16U) |
        ((uint32_t) input[2] << 8U) |
        (uint32_t) input[3];
}

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t) (value >> 8U);
    output[1] = (uint8_t) value;
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t) (value >> 24U);
    output[1] = (uint8_t) (value >> 16U);
    output[2] = (uint8_t) (value >> 8U);
    output[3] = (uint8_t) value;
}

static bool constant_time_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t size)
{
    uint8_t difference = 0U;
    for (size_t index = 0U; index < size; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static esp_err_t calculate_fingerprint(
    const uint8_t psk[AIRDAP_NETWORK_AUTH_PSK_SIZE],
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE])
{
    uint8_t material[sizeof(fingerprint_label) - 1U +
        PSK_IDENTITY_SIZE - 1U + AIRDAP_NETWORK_AUTH_PSK_SIZE];
    size_t material_size = 0U;
    memcpy(material + material_size, fingerprint_label,
        sizeof(fingerprint_label) - 1U);
    material_size += sizeof(fingerprint_label) - 1U;
    const size_t identity_size = strlen(active_identity);
    memcpy(material + material_size, active_identity, identity_size);
    material_size += identity_size;
    memcpy(material + material_size, psk, AIRDAP_NETWORK_AUTH_PSK_SIZE);
    material_size += AIRDAP_NETWORK_AUTH_PSK_SIZE;

    size_t fingerprint_size = 0U;
    const psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        material,
        material_size,
        fingerprint,
        AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE,
        &fingerprint_size);
    clear_bytes(material, sizeof(material));
    return status == PSA_SUCCESS &&
        fingerprint_size == AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE
        ? ESP_OK
        : ESP_FAIL;
}

static void encode_credential_record(
    const active_credential_t *credential,
    uint8_t output[CREDENTIAL_RECORD_SIZE])
{
    memset(output, 0, CREDENTIAL_RECORD_SIZE);
    memcpy(output, credential_magic, sizeof(credential_magic));
    output[4] = CREDENTIAL_RECORD_VERSION;
    write_u16_be(output + 6U, CREDENTIAL_RECORD_SIZE);
    write_u32_be(
        output + CREDENTIAL_RECORD_GENERATION_OFFSET,
        credential->generation);
    memcpy(
        output + CREDENTIAL_RECORD_PSK_OFFSET,
        credential->psk,
        sizeof(credential->psk));
}

static esp_err_t decode_credential_record(
    const uint8_t *input,
    size_t input_size,
    active_credential_t *credential)
{
    if (input == NULL || credential == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (input_size != CREDENTIAL_RECORD_SIZE ||
        read_u16_be(input + 6U) != CREDENTIAL_RECORD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(input, credential_magic, sizeof(credential_magic)) != 0 ||
        input[5] != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (input[4] != CREDENTIAL_RECORD_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    const uint32_t generation = read_u32_be(
        input + CREDENTIAL_RECORD_GENERATION_OFFSET);
    if (generation == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    active_credential_t candidate = {
        .valid = true,
        .generation = generation,
    };
    memcpy(
        candidate.psk,
        input + CREDENTIAL_RECORD_PSK_OFFSET,
        sizeof(candidate.psk));
    const esp_err_t fingerprint_error = calculate_fingerprint(
        candidate.psk,
        candidate.fingerprint);
    if (fingerprint_error != ESP_OK) {
        clear_bytes(&candidate, sizeof(candidate));
        return fingerprint_error;
    }
    *credential = candidate;
    clear_bytes(&candidate, sizeof(candidate));
    return ESP_OK;
}

static uint32_t allocate_session_id(void)
{
    const uint32_t allocated = next_session_id;
    ++next_session_id;
    if (next_session_id == 0U) {
        next_session_id = 1U;
    }
    return allocated;
}

static pending_revoke_t invalidate_owner_locked(void)
{
    pending_revoke_t pending = {0};
    if (!owner_session.active) {
        return pending;
    }
    pending.handler = revoke_handler;
    pending.context = revoke_context;
    pending.session_id = owner_session.session_id;
    clear_bytes(&owner_session, sizeof(owner_session));
    return pending;
}

static void invoke_revoke(const pending_revoke_t *pending)
{
    if (pending->handler != NULL && pending->session_id != 0U) {
        pending->handler(pending->context, pending->session_id);
    }
}

static bool owner_expired_at(int64_t now_us)
{
    return owner_session.active && now_us >= owner_session.last_activity_us &&
        now_us - owner_session.last_activity_us >=
            AIRDAP_NETWORK_AUTH_SESSION_IDLE_TIMEOUT_US;
}

static void expiry_timer_callback(void *argument)
{
    (void) argument;
    /* esp_timer callbacks must not block. Contention delays expiry until the
     * next one-second poll instead of stalling the shared timer task. */
    if (!try_lock_auth()) {
        return;
    }
    pending_revoke_t pending = {0};
    if (owner_expired_at(esp_timer_get_time())) {
        pending = invalidate_owner_locked();
    }
    unlock_auth();
    invoke_revoke(&pending);
}

esp_err_t airdap_network_auth_init(void)
{
    if (initialized || auth_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const airdap_device_identity_t *identity = airdap_device_identity_get();
    if (identity == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int identity_length = snprintf(
        active_identity,
        sizeof(active_identity),
        "AIRDAP:%s",
        identity->device_id);
    if (identity_length < 0 || (size_t) identity_length >=
        sizeof(active_identity)) {
        clear_bytes(active_identity, sizeof(active_identity));
        return ESP_ERR_INVALID_SIZE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        clear_bytes(active_identity, sizeof(active_identity));
        return ESP_FAIL;
    }

    uint8_t record[CREDENTIAL_RECORD_SIZE];
    size_t record_size = sizeof(record);
    const esp_err_t load_error = airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        record,
        &record_size);
    if (load_error == ESP_OK) {
        const esp_err_t decode_error = decode_credential_record(
            record,
            record_size,
            &active_credential);
        clear_bytes(record, sizeof(record));
        if (decode_error != ESP_OK) {
            clear_bytes(active_identity, sizeof(active_identity));
            return decode_error;
        }
    } else if (load_error != ESP_ERR_NOT_FOUND) {
        clear_bytes(record, sizeof(record));
        clear_bytes(active_identity, sizeof(active_identity));
        return load_error;
    } else {
        clear_bytes(record, sizeof(record));
    }

    auth_mutex = xSemaphoreCreateMutex();
    if (auth_mutex == NULL) {
        clear_bytes(&active_credential, sizeof(active_credential));
        clear_bytes(active_identity, sizeof(active_identity));
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = expiry_timer_callback,
        .name = "airdap_auth_expiry",
    };
    esp_err_t error = esp_timer_create(&timer_args, &expiry_timer);
    if (error == ESP_OK) {
        error = esp_timer_start_periodic(
            expiry_timer,
            AIRDAP_NETWORK_AUTH_EXPIRY_POLL_US);
    }
    if (error != ESP_OK) {
        clear_bytes(&active_credential, sizeof(active_credential));
        clear_bytes(active_identity, sizeof(active_identity));
        return error;
    }
    initialized = true;
    return ESP_OK;
}

esp_err_t airdap_network_auth_set_revoke_handler(
    airdap_network_auth_revoke_fn handler,
    void *context)
{
    if (!initialized || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_auth()) {
        return ESP_FAIL;
    }
    if (revoke_handler != NULL) {
        unlock_auth();
        return ESP_ERR_INVALID_STATE;
    }
    revoke_handler = handler;
    revoke_context = context;
    unlock_auth();
    return ESP_OK;
}

airdap_network_auth_result_t airdap_network_auth_pair(
    const uint8_t *request,
    size_t request_size,
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE])
{
    if (request == NULL || fingerprint == NULL ||
        request_size != AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE) {
        return AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT;
    }
    if (!initialized) {
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }
    if (request[0] != AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION) {
        return AIRDAP_NETWORK_AUTH_UNSUPPORTED_VERSION;
    }
    if (!lock_auth()) {
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }

    const uint8_t *candidate_psk = request + 1U;
    if (active_credential.valid && constant_time_equal(
            active_credential.psk,
            candidate_psk,
            AIRDAP_NETWORK_AUTH_PSK_SIZE)) {
        memcpy(fingerprint, active_credential.fingerprint,
            AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE);
        unlock_auth();
        return AIRDAP_NETWORK_AUTH_OK;
    }

    active_credential_t candidate = {
        .valid = true,
        .generation = active_credential.valid
            ? active_credential.generation + 1U
            : 1U,
    };
    if (candidate.generation == 0U) {
        candidate.generation = 1U;
    }
    memcpy(candidate.psk, candidate_psk, sizeof(candidate.psk));
    if (calculate_fingerprint(candidate.psk, candidate.fingerprint) != ESP_OK) {
        clear_bytes(&candidate, sizeof(candidate));
        unlock_auth();
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }

    uint8_t record[CREDENTIAL_RECORD_SIZE];
    encode_credential_record(&candidate, record);
    const esp_err_t store_error = airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_AUTH_MATERIAL,
        record,
        sizeof(record));
    clear_bytes(record, sizeof(record));
    if (store_error != ESP_OK) {
        clear_bytes(&candidate, sizeof(candidate));
        unlock_auth();
        return AIRDAP_NETWORK_AUTH_STORAGE_FAILED;
    }

    pending_revoke_t pending = invalidate_owner_locked();
    clear_bytes(&active_credential, sizeof(active_credential));
    active_credential = candidate;
    memcpy(fingerprint, candidate.fingerprint,
        AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE);
    clear_bytes(&candidate, sizeof(candidate));
    unlock_auth();
    invoke_revoke(&pending);
    return AIRDAP_NETWORK_AUTH_OK;
}

static void release_handshake_slot(void)
{
    if (!lock_auth()) {
        return;
    }
    if (pending_handshakes > 0U) {
        --pending_handshakes;
    }
    unlock_auth();
}

static int complete_tls_handshake(
    esp_tls_cfg_server_t *config,
    int socket_fd,
    esp_tls_t *tls)
{
    int result = esp_tls_server_session_init(config, socket_fd, tls);
    if (result != ESP_OK) {
        return result;
    }
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t) AIRDAP_NETWORK_AUTH_TLS_HANDSHAKE_TIMEOUT_MS * 1000;
    for (;;) {
        result = esp_tls_server_session_continue_async(tls);
        if (result == ESP_OK) {
            return esp_tls_set_conn_state(tls, ESP_TLS_DONE);
        }
        if (result != ESP_TLS_ERR_SSL_WANT_READ &&
            result != ESP_TLS_ERR_SSL_WANT_WRITE) {
            return result;
        }

        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return ESP_FAIL;
        }
        struct pollfd descriptor = {
            .fd = socket_fd,
            .events = result == ESP_TLS_ERR_SSL_WANT_READ
                ? POLLIN
                : POLLOUT,
        };
        const int timeout_ms = (int) ((remaining_us + 999) / 1000);
        const int ready = poll(&descriptor, 1U, timeout_ms);
        if (ready == 0) {
            return ESP_FAIL;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ESP_FAIL;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return ESP_FAIL;
        }
    }
}

airdap_network_auth_result_t airdap_network_auth_tls_accept(
    int socket_fd,
    airdap_network_auth_connection_t **connection)
{
    if (connection == NULL || socket_fd < 0) {
        return AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT;
    }
    *connection = NULL;
    if (!initialized) {
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }

    airdap_network_auth_connection_t *candidate = calloc(1U, sizeof(*candidate));
    if (candidate == NULL) {
        return AIRDAP_NETWORK_AUTH_NO_MEMORY;
    }
    if (!lock_auth()) {
        free(candidate);
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }
    if (!active_credential.valid) {
        unlock_auth();
        free(candidate);
        return AIRDAP_NETWORK_AUTH_NO_CREDENTIAL;
    }
    if (pending_handshakes >= AIRDAP_NETWORK_AUTH_MAX_PENDING_HANDSHAKES) {
        unlock_auth();
        free(candidate);
        return AIRDAP_NETWORK_AUTH_BUSY;
    }
    ++pending_handshakes;
    candidate->credential_generation = active_credential.generation;
    memcpy(candidate->psk, active_credential.psk, sizeof(candidate->psk));
    memcpy(candidate->credential_fingerprint,
        active_credential.fingerprint,
        sizeof(candidate->credential_fingerprint));
    memcpy(candidate->psk_identity, active_identity, sizeof(active_identity));
    unlock_auth();

    const int socket_flags = fcntl(socket_fd, F_GETFL);
    if (socket_flags < 0) {
        release_handshake_slot();
        clear_bytes(candidate, sizeof(*candidate));
        free(candidate);
        return AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT;
    }
    const bool restore_socket_flags = (socket_flags & O_NONBLOCK) == 0;
    if (restore_socket_flags &&
        fcntl(socket_fd, F_SETFL, socket_flags | O_NONBLOCK) != 0) {
        release_handshake_slot();
        clear_bytes(candidate, sizeof(*candidate));
        free(candidate);
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }

    candidate->psk_hint_key = (psk_hint_key_t) {
        .key = candidate->psk,
        .key_size = sizeof(candidate->psk),
        .hint = candidate->psk_identity,
    };
    candidate->tls = esp_tls_init();
    if (candidate->tls == NULL) {
        if (restore_socket_flags) {
            (void) fcntl(socket_fd, F_SETFL, socket_flags);
        }
        release_handshake_slot();
        clear_bytes(candidate, sizeof(*candidate));
        free(candidate);
        return AIRDAP_NETWORK_AUTH_NO_MEMORY;
    }
    esp_tls_cfg_server_t config = {
        .tls_handshake_timeout_ms =
            AIRDAP_NETWORK_AUTH_TLS_HANDSHAKE_TIMEOUT_MS,
        .psk_hint_key = &candidate->psk_hint_key,
        .tls_version = ESP_TLS_VER_TLS_1_3,
        .ciphersuites_list = tls_ciphersuites,
    };
    const int handshake_error = complete_tls_handshake(
        &config,
        socket_fd,
        candidate->tls);
    const bool socket_mode_restored = !restore_socket_flags ||
        fcntl(socket_fd, F_SETFL, socket_flags) == 0;

    if (!lock_auth()) {
        release_handshake_slot();
        esp_tls_server_session_delete(candidate->tls);
        clear_bytes(candidate, sizeof(*candidate));
        free(candidate);
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }
    if (pending_handshakes > 0U) {
        --pending_handshakes;
    }
    const bool still_current = active_credential.valid &&
        active_credential.generation == candidate->credential_generation &&
        constant_time_equal(
            active_credential.fingerprint,
            candidate->credential_fingerprint,
            AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE);
    unlock_auth();

    if (handshake_error != 0 || !socket_mode_restored || !still_current) {
        esp_tls_server_session_delete(candidate->tls);
        clear_bytes(candidate, sizeof(*candidate));
        free(candidate);
        if (handshake_error != 0) {
            return AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED;
        }
        return !socket_mode_restored
            ? AIRDAP_NETWORK_AUTH_INVALID_STATE
            : AIRDAP_NETWORK_AUTH_EXPIRED;
    }
    candidate->tls_authenticated = true;
    *connection = candidate;
    return AIRDAP_NETWORK_AUTH_OK;
}

ssize_t airdap_network_auth_tls_read(
    airdap_network_auth_connection_t *connection,
    void *buffer,
    size_t length)
{
    if (connection == NULL || connection->tls == NULL || buffer == NULL ||
        length == 0U) {
        return -1;
    }
    return esp_tls_conn_read(connection->tls, buffer, length);
}

ssize_t airdap_network_auth_tls_write(
    airdap_network_auth_connection_t *connection,
    const void *buffer,
    size_t length)
{
    if (connection == NULL || connection->tls == NULL || buffer == NULL ||
        length == 0U) {
        return -1;
    }
    return esp_tls_conn_write(connection->tls, buffer, length);
}

static void copy_session_info(airdap_network_auth_session_info_t *session_info)
{
    session_info->session_id = owner_session.session_id;
    session_info->credential_generation = owner_session.credential_generation;
    memcpy(session_info->session_token, owner_session.token,
        sizeof(session_info->session_token));
    memcpy(session_info->credential_fingerprint,
        owner_session.credential_fingerprint,
        sizeof(session_info->credential_fingerprint));
}

airdap_network_auth_result_t airdap_network_auth_session_bind(
    airdap_network_auth_connection_t *connection,
    const uint8_t *owner_token,
    size_t owner_token_size,
    airdap_network_auth_session_info_t *session_info)
{
    if (connection == NULL || session_info == NULL ||
        ((owner_token == NULL) != (owner_token_size == 0U)) ||
        (owner_token != NULL &&
            owner_token_size != AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE)) {
        return AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT;
    }
    if (!connection->tls_authenticated) {
        return AIRDAP_NETWORK_AUTH_UNAUTHENTICATED;
    }
    if (connection->bound) {
        return AIRDAP_NETWORK_AUTH_REPLAY;
    }
    if (!lock_auth()) {
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }

    pending_revoke_t pending = {0};
    const int64_t now_us = esp_timer_get_time();
    if (owner_expired_at(now_us)) {
        pending = invalidate_owner_locked();
    }
    if (!active_credential.valid ||
        active_credential.generation != connection->credential_generation ||
        !constant_time_equal(
            active_credential.fingerprint,
            connection->credential_fingerprint,
            AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE)) {
        unlock_auth();
        invoke_revoke(&pending);
        return AIRDAP_NETWORK_AUTH_EXPIRED;
    }

    airdap_network_auth_result_t result = AIRDAP_NETWORK_AUTH_OK;
    if (owner_session.active) {
        if (owner_token == NULL || !constant_time_equal(
                owner_session.token,
                owner_token,
                AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE)) {
            result = AIRDAP_NETWORK_AUTH_BUSY;
        }
    } else if (owner_token != NULL) {
        result = AIRDAP_NETWORK_AUTH_EXPIRED;
    } else {
        owner_session.active = true;
        owner_session.session_id = allocate_session_id();
        owner_session.credential_generation = connection->credential_generation;
        esp_fill_random(owner_session.token, sizeof(owner_session.token));
        memcpy(owner_session.credential_fingerprint,
            connection->credential_fingerprint,
            sizeof(owner_session.credential_fingerprint));
    }

    if (result == AIRDAP_NETWORK_AUTH_OK) {
        owner_session.last_activity_us = now_us;
        connection->bound = true;
        connection->session_id = owner_session.session_id;
        copy_session_info(session_info);
    }
    unlock_auth();
    invoke_revoke(&pending);
    return result;
}

airdap_network_auth_result_t airdap_network_auth_session_validate(
    airdap_network_auth_connection_t *connection,
    uint32_t session_id)
{
    if (connection == NULL || session_id == 0U) {
        return AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT;
    }
    if (!connection->tls_authenticated || !connection->bound) {
        return AIRDAP_NETWORK_AUTH_UNAUTHENTICATED;
    }
    if (!lock_auth()) {
        return AIRDAP_NETWORK_AUTH_INVALID_STATE;
    }
    pending_revoke_t pending = {0};
    const int64_t now_us = esp_timer_get_time();
    if (owner_expired_at(now_us)) {
        pending = invalidate_owner_locked();
    }
    const bool valid = owner_session.active &&
        session_id == connection->session_id &&
        session_id == owner_session.session_id &&
        connection->credential_generation ==
            owner_session.credential_generation &&
        constant_time_equal(
            connection->credential_fingerprint,
            owner_session.credential_fingerprint,
            AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE);
    if (valid) {
        owner_session.last_activity_us = now_us;
    }
    unlock_auth();
    invoke_revoke(&pending);
    return valid ? AIRDAP_NETWORK_AUTH_OK : AIRDAP_NETWORK_AUTH_EXPIRED;
}

void airdap_network_auth_connection_close(
    airdap_network_auth_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    pending_revoke_t pending = {0};
    if (initialized && connection->bound && lock_auth()) {
        if (owner_session.active &&
            owner_session.session_id == connection->session_id) {
            pending = invalidate_owner_locked();
        }
        unlock_auth();
    }
    if (connection->tls != NULL) {
        esp_tls_server_session_delete(connection->tls);
    }
    clear_bytes(connection, sizeof(*connection));
    free(connection);
    invoke_revoke(&pending);
}

esp_err_t airdap_network_auth_clear_network_configuration(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_auth()) {
        return ESP_FAIL;
    }
    const esp_err_t error = airdap_config_store_clear(
        AIRDAP_CONFIG_CLEAR_NETWORK);
    if (error != ESP_OK) {
        unlock_auth();
        return error;
    }
    pending_revoke_t pending = invalidate_owner_locked();
    clear_bytes(&active_credential, sizeof(active_credential));
    unlock_auth();
    invoke_revoke(&pending);
    return ESP_OK;
}
