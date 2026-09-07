#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>

#include "airdap_config_store.h"
#include "airdap_device_identity.h"
#include "airdap_network_auth.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/semphr.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "psa/crypto.h"

struct fake_timer {
    void (*callback)(void *argument);
    void *argument;
    uint64_t period_us;
};

struct fake_tls {
    unsigned marker;
    esp_tls_conn_state_t state;
};

static const airdap_device_identity_t identity = {
    .device_id = "ADP-001122334455",
};
static struct fake_timer expiry_timer;
static uint8_t stored_record[128];
static size_t stored_record_size;
static unsigned store_writes;
static bool fail_store_write;
static bool fail_store_clear;
static int64_t now_us;
static uint8_t random_seed = 0x20U;
static int handshake_result;
static unsigned tls_created;
static unsigned tls_deleted;
static unsigned handshake_calls;
static unsigned tls_done_transitions;
static unsigned revoke_count;
static uint32_t revoked_session;
static int nested_handshake_depth;
static bool rotate_during_handshake;
static uint8_t rotated_key[AIRDAP_NETWORK_AUTH_PSK_SIZE];
static bool socket_nonblocking;
static unsigned poll_calls;
static int poll_result;
static int64_t poll_elapsed_us;
static bool handshake_succeeds_after_poll;
static unsigned handshake_continue_calls;
static int64_t handshake_init_delay_us;
static int64_t handshake_continue_delay_us;
static pthread_mutex_t interleaving_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interleaving_condition = PTHREAD_COND_INITIALIZER;
static bool interleave_clear_and_pair;
static bool clear_commit_entered;
static bool pair_auth_probe_complete;
static bool pair_was_blocked_by_clear;

int fcntl(int file_descriptor, int command, ...)
{
    assert(file_descriptor >= 0);
    if (command == F_GETFL) {
        return socket_nonblocking ? O_NONBLOCK : 0;
    }
    assert(command == F_SETFL);
    va_list arguments;
    va_start(arguments, command);
    const int flags = va_arg(arguments, int);
    va_end(arguments);
    socket_nonblocking = (flags & O_NONBLOCK) != 0;
    return 0;
}

int poll(struct pollfd *descriptors, nfds_t count, int timeout_ms)
{
    assert(descriptors != NULL && count == 1U);
    assert(descriptors[0].fd >= 0);
    assert(descriptors[0].events == POLLIN ||
        descriptors[0].events == POLLOUT);
    assert(socket_nonblocking);
    assert(timeout_ms > 0 && timeout_ms <=
        AIRDAP_NETWORK_AUTH_TLS_HANDSHAKE_TIMEOUT_MS);
    ++poll_calls;
    now_us += poll_elapsed_us > 0
        ? poll_elapsed_us
        : (int64_t) timeout_ms * 1000;
    descriptors[0].revents = 0;
    return poll_result;
}

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return &identity;
}

esp_err_t airdap_config_store_get_blob(
    airdap_config_slot_t slot,
    void *output,
    size_t *inout_size)
{
    assert(slot == AIRDAP_CONFIG_SLOT_AUTH_MATERIAL);
    assert(inout_size != NULL);
    if (stored_record_size == 0U) {
        *inout_size = 0U;
        return ESP_ERR_NOT_FOUND;
    }
    if (output == NULL || *inout_size < stored_record_size) {
        *inout_size = stored_record_size;
        return output == NULL ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    memcpy(output, stored_record, stored_record_size);
    *inout_size = stored_record_size;
    return ESP_OK;
}

esp_err_t airdap_config_store_set_blob(
    airdap_config_slot_t slot,
    const void *data,
    size_t data_size)
{
    assert(slot == AIRDAP_CONFIG_SLOT_AUTH_MATERIAL);
    assert(data != NULL && data_size <= sizeof(stored_record));
    assert(pthread_mutex_lock(&interleaving_mutex) == 0);
    assert(!clear_commit_entered);
    assert(pthread_mutex_unlock(&interleaving_mutex) == 0);
    ++store_writes;
    if (fail_store_write) {
        return ESP_FAIL;
    }
    memcpy(stored_record, data, data_size);
    stored_record_size = data_size;
    return ESP_OK;
}

esp_err_t airdap_config_store_clear(uint32_t flags)
{
    assert(flags == AIRDAP_CONFIG_CLEAR_NETWORK);
    if (fail_store_clear) {
        return ESP_FAIL;
    }
    memset(stored_record, 0, sizeof(stored_record));
    stored_record_size = 0U;

    assert(pthread_mutex_lock(&interleaving_mutex) == 0);
    if (interleave_clear_and_pair) {
        clear_commit_entered = true;
        assert(pthread_cond_broadcast(&interleaving_condition) == 0);
        while (!pair_auth_probe_complete) {
            assert(pthread_cond_wait(
                &interleaving_condition,
                &interleaving_mutex) == 0);
        }
        clear_commit_entered = false;
    }
    assert(pthread_mutex_unlock(&interleaving_mutex) == 0);
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    assert(mutex != NULL);
    assert(pthread_mutex_init(mutex, NULL) == 0);
    return mutex;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned int timeout)
{
    (void) timeout;
    assert(pthread_mutex_lock(&interleaving_mutex) == 0);
    if (interleave_clear_and_pair && clear_commit_entered &&
        !pair_auth_probe_complete) {
        const int probe = pthread_mutex_trylock(semaphore);
        assert(probe == 0 || probe == EBUSY);
        pair_was_blocked_by_clear = probe == EBUSY;
        pair_auth_probe_complete = true;
        assert(pthread_cond_broadcast(&interleaving_condition) == 0);
        assert(pthread_mutex_unlock(&interleaving_mutex) == 0);
        if (probe == 0) {
            return pdTRUE;
        }
        return pthread_mutex_lock(semaphore) == 0 ? pdTRUE : pdFALSE;
    }
    assert(pthread_mutex_unlock(&interleaving_mutex) == 0);
    return pthread_mutex_lock(semaphore) == 0 ? pdTRUE : pdFALSE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return pthread_mutex_unlock(semaphore) == 0 ? pdTRUE : pdFALSE;
}

esp_err_t esp_timer_create(
    const esp_timer_create_args_t *args,
    esp_timer_handle_t *output)
{
    assert(args != NULL && args->callback != NULL && output != NULL);
    expiry_timer.callback = args->callback;
    expiry_timer.argument = args->arg;
    *output = &expiry_timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(
    esp_timer_handle_t timer,
    uint64_t period_us)
{
    assert(timer == &expiry_timer);
    timer->period_us = period_us;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return now_us;
}

void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = random_seed++;
    }
}

psa_status_t psa_crypto_init(void)
{
    return PSA_SUCCESS;
}

psa_status_t psa_hash_compute(
    psa_algorithm_t algorithm,
    const uint8_t *input,
    size_t input_length,
    uint8_t *hash,
    size_t hash_size,
    size_t *hash_length)
{
    assert(algorithm == PSA_ALG_SHA_256);
    size_t output_length = hash_size;
    const int result = EVP_Q_digest(
        NULL,
        "SHA256",
        NULL,
        input,
        input_length,
        hash,
        &output_length);
    if (result != 1) {
        return -1;
    }
    *hash_length = output_length;
    return PSA_SUCCESS;
}

esp_tls_t *esp_tls_init(void)
{
    struct fake_tls *tls = malloc(sizeof(*tls));
    if (tls != NULL) {
        tls->marker = ++tls_created;
        tls->state = ESP_TLS_INIT;
    }
    return tls;
}

static void assert_tls_config(const esp_tls_cfg_server_t *config)
{
    assert(config != NULL);
    assert(config->tls_handshake_timeout_ms ==
        AIRDAP_NETWORK_AUTH_TLS_HANDSHAKE_TIMEOUT_MS);
    assert(config->tls_version == ESP_TLS_VER_TLS_1_3);
    assert(config->ciphersuites_list != NULL);
    assert(config->ciphersuites_list[0] ==
        MBEDTLS_TLS1_3_AES_128_GCM_SHA256);
    assert(config->ciphersuites_list[1] == 0);
    assert(config->psk_hint_key != NULL);
    assert(config->psk_hint_key->key_size == AIRDAP_NETWORK_AUTH_PSK_SIZE);
    assert(stored_record_size == 44U);
    assert(memcmp(
        config->psk_hint_key->key,
        stored_record + 12U,
        AIRDAP_NETWORK_AUTH_PSK_SIZE) == 0);
    assert(strcmp(
        config->psk_hint_key->hint,
        "AIRDAP:ADP-001122334455") == 0);
}

int esp_tls_server_session_init(
    esp_tls_cfg_server_t *config,
    int socket_fd,
    esp_tls_t *tls)
{
    assert(socket_fd >= 0 && tls != NULL);
    assert(socket_nonblocking);
    assert_tls_config(config);
    ++handshake_calls;
    handshake_continue_calls = 0U;
    now_us += handshake_init_delay_us;

    if (rotate_during_handshake) {
        rotate_during_handshake = false;
        uint8_t request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE] = {
            AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION,
        };
        memcpy(request + 1U, rotated_key, sizeof(rotated_key));
        uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
        assert(airdap_network_auth_pair(
            request,
            sizeof(request),
            fingerprint) == AIRDAP_NETWORK_AUTH_OK);
    }

    if (nested_handshake_depth > 0) {
        const bool expect_busy = nested_handshake_depth == 1;
        --nested_handshake_depth;
        airdap_network_auth_connection_t *nested = NULL;
        const airdap_network_auth_result_t nested_result =
            airdap_network_auth_tls_accept(100 + nested_handshake_depth, &nested);
        if (expect_busy) {
            assert(nested_result == AIRDAP_NETWORK_AUTH_BUSY);
            assert(nested == NULL);
        } else {
            assert(nested_result == AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
        }
    }
    return ESP_OK;
}

int esp_tls_server_session_continue_async(esp_tls_t *tls)
{
    assert(tls != NULL && socket_nonblocking);
    ++handshake_continue_calls;
    now_us += handshake_continue_delay_us;
    if (handshake_succeeds_after_poll && handshake_continue_calls > 1U) {
        return ESP_OK;
    }
    return handshake_result;
}

int esp_tls_set_conn_state(esp_tls_t *tls, esp_tls_conn_state_t conn_state)
{
    assert(tls != NULL && conn_state == ESP_TLS_DONE);
    tls->state = conn_state;
    ++tls_done_transitions;
    return ESP_OK;
}

void esp_tls_server_session_delete(esp_tls_t *tls)
{
    assert(tls != NULL);
    ++tls_deleted;
    free(tls);
}

ssize_t esp_tls_conn_read(esp_tls_t *tls, void *buffer, size_t length)
{
    assert(tls != NULL && tls->state == ESP_TLS_DONE && buffer != NULL);
    memset(buffer, 0xA5, length);
    return (ssize_t) length;
}

ssize_t esp_tls_conn_write(
    esp_tls_t *tls,
    const void *buffer,
    size_t length)
{
    assert(tls != NULL && tls->state == ESP_TLS_DONE && buffer != NULL);
    return (ssize_t) length;
}

static void revoked(void *context, uint32_t session_id)
{
    assert(context == &revoke_count);
    ++revoke_count;
    revoked_session = session_id;
}

static void make_pair_request(
    uint8_t seed,
    uint8_t request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE])
{
    request[0] = AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION;
    for (size_t index = 1U; index < AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE;
         ++index) {
        request[index] = (uint8_t) (seed + index - 1U);
    }
}

static airdap_network_auth_connection_t *accept_connection(int socket_fd)
{
    airdap_network_auth_connection_t *connection = NULL;
    handshake_result = 0;
    const unsigned transitions_before = tls_done_transitions;
    assert(airdap_network_auth_tls_accept(socket_fd, &connection) ==
        AIRDAP_NETWORK_AUTH_OK);
    assert(connection != NULL);
    assert(!socket_nonblocking);
    assert(tls_done_transitions == transitions_before + 1U);
    return connection;
}

typedef struct {
    uint8_t request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE];
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    airdap_network_auth_result_t result;
} pair_thread_arguments_t;

static void *clear_network_worker(void *argument)
{
    esp_err_t *result = argument;
    *result = airdap_network_auth_clear_network_configuration();
    return NULL;
}

static void *pair_worker(void *argument)
{
    pair_thread_arguments_t *pair = argument;
    pair->result = airdap_network_auth_pair(
        pair->request,
        sizeof(pair->request),
        pair->fingerprint);
    return NULL;
}

static void test_pairing_is_atomic_idempotent_and_versioned(void)
{
    static const uint8_t expected_fingerprint[
        AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE] = {
        0xEE, 0x83, 0xDA, 0x8D, 0x15, 0x26, 0x9C, 0x54,
        0xE2, 0xF7, 0xC5, 0x47, 0xB5, 0x55, 0x58, 0xE1,
        0x2D, 0x35, 0xC7, 0x78, 0x57, 0x6E, 0x0B, 0x26,
        0xF2, 0x1B, 0x0B, 0x3C, 0x80, 0xA6, 0x51, 0x34,
    };
    uint8_t request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE];
    uint8_t first_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    uint8_t retry_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    make_pair_request(0U, request);

    assert(airdap_network_auth_pair(NULL, sizeof(request), first_fingerprint) ==
        AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT);
    assert(airdap_network_auth_pair(request, sizeof(request) - 1U,
        first_fingerprint) == AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT);
    request[0] = 2U;
    assert(airdap_network_auth_pair(request, sizeof(request), first_fingerprint) ==
        AIRDAP_NETWORK_AUTH_UNSUPPORTED_VERSION);
    request[0] = AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION;

    fail_store_write = true;
    assert(airdap_network_auth_pair(request, sizeof(request), first_fingerprint) ==
        AIRDAP_NETWORK_AUTH_STORAGE_FAILED);
    fail_store_write = false;
    assert(stored_record_size == 0U);

    assert(airdap_network_auth_pair(request, sizeof(request), first_fingerprint) ==
        AIRDAP_NETWORK_AUTH_OK);
    assert(memcmp(first_fingerprint, expected_fingerprint,
        sizeof(expected_fingerprint)) == 0);
    assert(stored_record_size > AIRDAP_NETWORK_AUTH_PSK_SIZE);
    assert(store_writes == 2U);
    assert(airdap_network_auth_pair(request, sizeof(request), retry_fingerprint) ==
        AIRDAP_NETWORK_AUTH_OK);
    assert(store_writes == 2U);
    assert(memcmp(first_fingerprint, retry_fingerprint,
        sizeof(first_fingerprint)) == 0);
}

static void test_handshake_failure_staleness_and_flood_cleanup(void)
{
    airdap_network_auth_connection_t *connection = NULL;

    /* A wrong key and a truncated ClientHello are deliberately indistinguishable
     * at the component boundary and must take the same cleanup path. */
    handshake_result = -0x7100;
    assert(airdap_network_auth_tls_accept(1, &connection) ==
        AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
    assert(!socket_nonblocking);
    assert(connection == NULL && tls_created == tls_deleted);
    handshake_result = -0x7280;
    assert(airdap_network_auth_tls_accept(2, &connection) ==
        AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
    assert(!socket_nonblocking);
    assert(connection == NULL && tls_created == tls_deleted);

    const unsigned polls_before_stall = poll_calls;
    handshake_result = ESP_TLS_ERR_SSL_WANT_READ;
    assert(airdap_network_auth_tls_accept(3, &connection) ==
        AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
    assert(poll_calls == polls_before_stall + 1U);
    assert(!socket_nonblocking);
    assert(connection == NULL && tls_created == tls_deleted);

    /* Two in-flight handshakes consume the bounded admission slots. The
     * recursive third attempt must be rejected before esp_tls allocates. */
    handshake_result = -1;
    nested_handshake_depth = 2;
    assert(airdap_network_auth_tls_accept(4, &connection) ==
        AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
    assert(connection == NULL && tls_created == tls_deleted);

    for (size_t index = 0U; index < sizeof(rotated_key); ++index) {
        rotated_key[index] = (uint8_t) (0x70U + index);
    }
    rotate_during_handshake = true;
    handshake_result = 0;
    assert(airdap_network_auth_tls_accept(5, &connection) ==
        AIRDAP_NETWORK_AUTH_EXPIRED);
    assert(connection == NULL && tls_created == tls_deleted);

    connection = accept_connection(6);
    uint8_t io[4];
    assert(airdap_network_auth_tls_read(connection, io, sizeof(io)) == 4);
    assert(airdap_network_auth_tls_write(connection, io, sizeof(io)) == 4);
    airdap_network_auth_connection_close(connection);
    assert(tls_created == tls_deleted);
}

static void reset_handshake_deadline_controls(void)
{
    poll_result = 0;
    poll_elapsed_us = 0;
    handshake_succeeds_after_poll = false;
    handshake_init_delay_us = 0;
    handshake_continue_delay_us = 0;
}

static void test_handshake_deadline_is_absolute(void)
{
    airdap_network_auth_connection_t *connection = NULL;
    const int64_t timeout_us =
        (int64_t) AIRDAP_NETWORK_AUTH_TLS_HANDSHAKE_TIMEOUT_MS * 1000;

    now_us = 1000;
    handshake_result = ESP_TLS_ERR_SSL_WANT_READ;
    poll_result = 1;
    poll_elapsed_us = timeout_us + 1;
    handshake_succeeds_after_poll = true;
    assert(airdap_network_auth_tls_accept(7, &connection) ==
        AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
    assert(connection == NULL && handshake_continue_calls == 1U);
    reset_handshake_deadline_controls();

    now_us = 2000;
    handshake_result = ESP_OK;
    handshake_init_delay_us = timeout_us + 1;
    assert(airdap_network_auth_tls_accept(8, &connection) ==
        AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
    assert(connection == NULL && handshake_continue_calls == 0U);
    reset_handshake_deadline_controls();

    now_us = 3000;
    handshake_result = ESP_OK;
    handshake_continue_delay_us = timeout_us + 1;
    assert(airdap_network_auth_tls_accept(9, &connection) ==
        AIRDAP_NETWORK_AUTH_AUTHENTICATION_FAILED);
    assert(connection == NULL && handshake_continue_calls == 1U);
    reset_handshake_deadline_controls();

    assert(tls_created == tls_deleted);
}

static void test_single_owner_join_replay_rotation_and_repair(void)
{
    airdap_network_auth_connection_t *owner = accept_connection(10);
    airdap_network_auth_connection_t *joiner = accept_connection(11);
    airdap_network_auth_connection_t *other = accept_connection(12);
    airdap_network_auth_session_info_t owner_info;
    airdap_network_auth_session_info_t join_info;

    now_us = 1000;
    assert(airdap_network_auth_session_bind(
        owner,
        NULL,
        0U,
        &owner_info) == AIRDAP_NETWORK_AUTH_OK);
    assert(owner_info.session_id != 0U);
    assert(airdap_network_auth_session_bind(
        owner,
        NULL,
        0U,
        &join_info) == AIRDAP_NETWORK_AUTH_REPLAY);

    uint8_t wrong_token[AIRDAP_NETWORK_AUTH_SESSION_TOKEN_SIZE] = {0};
    assert(airdap_network_auth_session_bind(
        other,
        wrong_token,
        sizeof(wrong_token),
        &join_info) == AIRDAP_NETWORK_AUTH_BUSY);
    assert(airdap_network_auth_session_bind(
        joiner,
        owner_info.session_token,
        sizeof(owner_info.session_token),
        &join_info) == AIRDAP_NETWORK_AUTH_OK);
    assert(join_info.session_id == owner_info.session_id);
    assert(memcmp(join_info.session_token, owner_info.session_token,
        sizeof(owner_info.session_token)) == 0);
    assert(airdap_network_auth_session_validate(owner, owner_info.session_id) ==
        AIRDAP_NETWORK_AUTH_OK);
    assert(airdap_network_auth_session_validate(other, owner_info.session_id) ==
        AIRDAP_NETWORK_AUTH_UNAUTHENTICATED);

    uint8_t replacement[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE];
    uint8_t replacement_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    make_pair_request(0xA0U, replacement);
    uint8_t record_before_failed_rotation[sizeof(stored_record)];
    memcpy(record_before_failed_rotation, stored_record, sizeof(stored_record));
    const size_t record_size_before_failed_rotation = stored_record_size;
    fail_store_write = true;
    assert(airdap_network_auth_pair(
        replacement,
        sizeof(replacement),
        replacement_fingerprint) == AIRDAP_NETWORK_AUTH_STORAGE_FAILED);
    fail_store_write = false;
    assert(stored_record_size == record_size_before_failed_rotation);
    assert(memcmp(
        stored_record,
        record_before_failed_rotation,
        sizeof(stored_record)) == 0);
    assert(revoke_count == 0U);
    assert(airdap_network_auth_session_validate(owner, owner_info.session_id) ==
        AIRDAP_NETWORK_AUTH_OK);
    airdap_network_auth_connection_t *retained = accept_connection(13);
    airdap_network_auth_connection_close(retained);

    assert(airdap_network_auth_pair(
        replacement,
        sizeof(replacement),
        replacement_fingerprint) == AIRDAP_NETWORK_AUTH_OK);
    assert(revoke_count == 1U && revoked_session == owner_info.session_id);
    assert(airdap_network_auth_session_validate(owner, owner_info.session_id) ==
        AIRDAP_NETWORK_AUTH_EXPIRED);
    assert(airdap_network_auth_session_validate(joiner, owner_info.session_id) ==
        AIRDAP_NETWORK_AUTH_EXPIRED);

    /* An old host becomes valid again only after explicitly pairing its old
     * key as a new generation. */
    uint8_t old_request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE];
    uint8_t ignored_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    make_pair_request(0U, old_request);
    assert(airdap_network_auth_pair(old_request, sizeof(old_request),
        ignored_fingerprint) == AIRDAP_NETWORK_AUTH_OK);
    airdap_network_auth_connection_t *repaired = accept_connection(14);
    airdap_network_auth_session_info_t repaired_info;
    assert(airdap_network_auth_session_bind(
        repaired,
        NULL,
        0U,
        &repaired_info) == AIRDAP_NETWORK_AUTH_OK);

    airdap_network_auth_connection_close(repaired);
    assert(revoke_count == 2U && revoked_session == repaired_info.session_id);

    airdap_network_auth_connection_close(owner);
    airdap_network_auth_connection_close(joiner);
    airdap_network_auth_connection_close(other);
}

static void test_timeout_stale_token_and_network_clear_release_owner(void)
{
    airdap_network_auth_connection_t *owner = accept_connection(20);
    airdap_network_auth_session_info_t info;
    now_us = 1000000;
    assert(airdap_network_auth_session_bind(owner, NULL, 0U, &info) ==
        AIRDAP_NETWORK_AUTH_OK);

    now_us += AIRDAP_NETWORK_AUTH_SESSION_IDLE_TIMEOUT_US;
    expiry_timer.callback(expiry_timer.argument);
    assert(revoke_count == 3U && revoked_session == info.session_id);
    assert(airdap_network_auth_session_validate(owner, info.session_id) ==
        AIRDAP_NETWORK_AUTH_EXPIRED);

    airdap_network_auth_connection_t *stale = accept_connection(21);
    airdap_network_auth_session_info_t ignored;
    assert(airdap_network_auth_session_bind(
        stale,
        info.session_token,
        sizeof(info.session_token),
        &ignored) == AIRDAP_NETWORK_AUTH_EXPIRED);
    assert(airdap_network_auth_session_bind(stale, NULL, 0U, &ignored) ==
        AIRDAP_NETWORK_AUTH_OK);
    fail_store_clear = true;
    assert(airdap_network_auth_clear_network_configuration() == ESP_FAIL);
    fail_store_clear = false;
    assert(stored_record_size == 44U);
    assert(revoke_count == 3U);
    assert(airdap_network_auth_session_validate(stale, ignored.session_id) ==
        AIRDAP_NETWORK_AUTH_OK);
    assert(airdap_network_auth_clear_network_configuration() == ESP_OK);
    assert(stored_record_size == 0U);
    assert(revoke_count == 4U && revoked_session == ignored.session_id);

    airdap_network_auth_connection_t *missing = NULL;
    assert(airdap_network_auth_tls_accept(22, &missing) ==
        AIRDAP_NETWORK_AUTH_NO_CREDENTIAL);
    assert(missing == NULL);

    airdap_network_auth_connection_close(owner);
    airdap_network_auth_connection_close(stale);
}

static void test_clear_and_pair_commit_are_serialized(void)
{
    uint8_t current_request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE];
    uint8_t current_fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    make_pair_request(0x30U, current_request);
    assert(airdap_network_auth_pair(
        current_request,
        sizeof(current_request),
        current_fingerprint) == AIRDAP_NETWORK_AUTH_OK);

    airdap_network_auth_connection_t *owner = accept_connection(30);
    airdap_network_auth_session_info_t owner_info;
    assert(airdap_network_auth_session_bind(owner, NULL, 0U, &owner_info) ==
        AIRDAP_NETWORK_AUTH_OK);

    pair_thread_arguments_t replacement = {0};
    make_pair_request(0x60U, replacement.request);
    esp_err_t clear_result = ESP_FAIL;
    pthread_t clear_thread;
    pthread_t pair_thread;

    assert(pthread_mutex_lock(&interleaving_mutex) == 0);
    interleave_clear_and_pair = true;
    clear_commit_entered = false;
    pair_auth_probe_complete = false;
    pair_was_blocked_by_clear = false;
    assert(pthread_mutex_unlock(&interleaving_mutex) == 0);

    assert(pthread_create(
        &clear_thread,
        NULL,
        clear_network_worker,
        &clear_result) == 0);
    assert(pthread_mutex_lock(&interleaving_mutex) == 0);
    while (!clear_commit_entered) {
        assert(pthread_cond_wait(
            &interleaving_condition,
            &interleaving_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&interleaving_mutex) == 0);

    assert(pthread_create(
        &pair_thread,
        NULL,
        pair_worker,
        &replacement) == 0);
    assert(pthread_join(clear_thread, NULL) == 0);
    assert(pthread_join(pair_thread, NULL) == 0);

    assert(pthread_mutex_lock(&interleaving_mutex) == 0);
    interleave_clear_and_pair = false;
    assert(pair_auth_probe_complete && pair_was_blocked_by_clear);
    assert(pthread_mutex_unlock(&interleaving_mutex) == 0);
    assert(clear_result == ESP_OK);
    assert(replacement.result == AIRDAP_NETWORK_AUTH_OK);
    assert(stored_record_size == 44U);
    assert(revoke_count == 5U && revoked_session == owner_info.session_id);

    airdap_network_auth_connection_t *replacement_connection =
        accept_connection(31);
    airdap_network_auth_session_info_t replacement_info;
    assert(airdap_network_auth_session_bind(
        replacement_connection,
        NULL,
        0U,
        &replacement_info) == AIRDAP_NETWORK_AUTH_OK);
    airdap_network_auth_connection_close(replacement_connection);
    airdap_network_auth_connection_close(owner);
}

static void prepare_persisted_record(uint32_t generation, uint8_t key_seed)
{
    static const uint8_t magic[] = {'A', 'N', 'E', 'T'};
    memset(stored_record, 0, sizeof(stored_record));
    memcpy(stored_record, magic, sizeof(magic));
    stored_record[4] = 1U;
    stored_record[6] = 0U;
    stored_record[7] = 44U;
    stored_record[8] = (uint8_t) (generation >> 24U);
    stored_record[9] = (uint8_t) (generation >> 16U);
    stored_record[10] = (uint8_t) (generation >> 8U);
    stored_record[11] = (uint8_t) generation;
    for (size_t index = 0U; index < AIRDAP_NETWORK_AUTH_PSK_SIZE; ++index) {
        stored_record[12U + index] = (uint8_t) (key_seed + index);
    }
    stored_record_size = 44U;
}

static void test_persisted_record_load(void)
{
    prepare_persisted_record(7U, 0U);
    assert(airdap_network_auth_init() == ESP_OK);

    uint8_t request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE];
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    make_pair_request(0U, request);
    assert(airdap_network_auth_pair(request, sizeof(request), fingerprint) ==
        AIRDAP_NETWORK_AUTH_OK);
    assert(store_writes == 0U);

    airdap_network_auth_connection_t *connection = accept_connection(30);
    airdap_network_auth_connection_close(connection);
    assert(tls_created == tls_deleted);
}

static void test_malformed_record_fails_closed(void)
{
    memset(stored_record, 0xA5, 44U);
    stored_record_size = 44U;
    assert(airdap_network_auth_init() != ESP_OK);
    airdap_network_auth_connection_t *connection = NULL;
    assert(airdap_network_auth_tls_accept(31, &connection) ==
        AIRDAP_NETWORK_AUTH_INVALID_STATE);
    assert(connection == NULL);
}

int main(int argument_count, char **arguments)
{
    if (argument_count == 2 && strcmp(arguments[1], "--loaded-init") == 0) {
        test_persisted_record_load();
        puts("Persisted network credential test passed");
        return 0;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--malformed-init") == 0) {
        test_malformed_record_fails_closed();
        puts("Malformed network credential test passed");
        return 0;
    }
    assert(argument_count == 1);
    assert(airdap_network_auth_init() == ESP_OK);
    assert(expiry_timer.period_us == AIRDAP_NETWORK_AUTH_EXPIRY_POLL_US);
    assert(airdap_network_auth_set_revoke_handler(revoked, &revoke_count) ==
        ESP_OK);

    test_pairing_is_atomic_idempotent_and_versioned();
    test_handshake_failure_staleness_and_flood_cleanup();
    test_handshake_deadline_is_absolute();
    test_single_owner_join_replay_rotation_and_repair();
    test_timeout_stale_token_and_network_clear_release_owner();
    test_clear_and_pair_commit_are_serialized();

    puts("Network authentication tests passed");
    return 0;
}
