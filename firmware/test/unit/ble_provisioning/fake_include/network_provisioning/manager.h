#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"
#include "esp_event.h"

extern esp_event_base_t NETWORK_PROV_EVENT;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
} wifi_sta_config_t;

typedef enum {
    NETWORK_PROV_INIT = 0,
    NETWORK_PROV_START,
    NETWORK_PROV_SET_WIFI_STA_CONFIG,
    NETWORK_PROV_WIFI_CRED_RECV,
    NETWORK_PROV_WIFI_CRED_FAIL,
    NETWORK_PROV_WIFI_CRED_SUCCESS,
    NETWORK_PROV_END,
    NETWORK_PROV_DEINIT,
} network_prov_cb_event_t;

typedef struct {
    void (*event_cb)(void *, network_prov_cb_event_t, void *);
    void *user_data;
} network_prov_event_handler_t;

#define NETWORK_PROV_EVENT_HANDLER_NONE { .event_cb = NULL, .user_data = NULL }

typedef struct {
    int marker;
} network_prov_scheme_t;

typedef struct {
    uint32_t wifi_conn_attempts;
} network_prov_wifi_conn_cfg_t;

typedef struct {
    network_prov_scheme_t scheme;
    network_prov_event_handler_t scheme_event_handler;
    network_prov_event_handler_t app_event_handler;
    network_prov_wifi_conn_cfg_t network_prov_wifi_conn_cfg;
} network_prov_mgr_config_t;

typedef enum {
    NETWORK_PROV_SECURITY_2 = 2,
} network_prov_security_t;

typedef struct {
    const char *salt;
    uint16_t salt_len;
    const char *verifier;
    uint16_t verifier_len;
} network_prov_security2_params_t;

typedef esp_err_t (*protocomm_req_handler_t)(
    uint32_t session_id,
    const uint8_t *input,
    ssize_t input_length,
    uint8_t **output,
    ssize_t *output_length,
    void *private_data);

esp_err_t network_prov_mgr_init(network_prov_mgr_config_t config);
esp_err_t network_prov_mgr_deinit(void);
esp_err_t network_prov_mgr_start_provisioning(
    network_prov_security_t security,
    const void *security_params,
    const char *service_name,
    const char *service_key);
void network_prov_mgr_stop_provisioning(void);
esp_err_t network_prov_mgr_endpoint_create(const char *endpoint_name);
esp_err_t network_prov_mgr_endpoint_register(
    const char *endpoint_name,
    protocomm_req_handler_t handler,
    void *user_context);
void network_prov_mgr_endpoint_unregister(const char *endpoint_name);
