#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct fake_tls esp_tls_t;

typedef struct {
    const uint8_t *key;
    size_t key_size;
    const char *hint;
} psk_hint_key_t;

typedef enum {
    ESP_TLS_VER_ANY = 0,
    ESP_TLS_VER_TLS_1_2 = 1,
    ESP_TLS_VER_TLS_1_3 = 2,
} esp_tls_proto_ver_t;

typedef enum {
    ESP_TLS_INIT = 0,
    ESP_TLS_CONNECTING,
    ESP_TLS_HANDSHAKE,
    ESP_TLS_FAIL,
    ESP_TLS_DONE,
} esp_tls_conn_state_t;

typedef struct {
    uint32_t tls_handshake_timeout_ms;
    const psk_hint_key_t *psk_hint_key;
    esp_tls_proto_ver_t tls_version;
    const int *ciphersuites_list;
} esp_tls_cfg_server_t;

#define ESP_TLS_ERR_SSL_WANT_READ (-0x6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-0x6880)

esp_tls_t *esp_tls_init(void);
int esp_tls_server_session_init(
    esp_tls_cfg_server_t *config,
    int socket_fd,
    esp_tls_t *tls);
int esp_tls_server_session_continue_async(esp_tls_t *tls);
int esp_tls_set_conn_state(esp_tls_t *tls, esp_tls_conn_state_t conn_state);
void esp_tls_server_session_delete(esp_tls_t *tls);
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *buffer, size_t length);
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *buffer, size_t length);
