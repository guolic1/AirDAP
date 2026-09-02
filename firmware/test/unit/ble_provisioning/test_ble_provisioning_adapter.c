#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_ble_provisioning.h"
#include "airdap_ble_provisioning_internal.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#include "airdap_sec2_credentials.h"
#include "airdap_wifi_manager.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

struct fake_esp_timer {
    void (*callback)(void *argument);
    void *argument;
    bool active;
    uint64_t timeout_us;
};

esp_event_base_t NETWORK_PROV_EVENT = "NETWORK_PROV_EVENT";
const network_prov_scheme_t network_prov_scheme_ble = {.marker = 42};

static const airdap_device_identity_t identity = {
    .device_id = "ADP-001122334455",
};
static struct fake_esp_timer timer;
static int fake_task;
static unsigned manager_init_count;
static unsigned manager_deinit_count;
static unsigned manager_start_count;
static unsigned manager_stop_count;
static unsigned prepare_count;
static unsigned stage_count;
static unsigned accept_count;
static unsigned finish_count;
static unsigned clear_count;
static unsigned restart_count;
static unsigned credential_load_count;
static esp_err_t prepare_result = ESP_OK;
static esp_err_t event_post_result = ESP_OK;
static bool prepared_after_manager_init;
static airdap_mode_event_t mode_events[32];
static size_t mode_event_count;
static airdap_wifi_credentials_t staged_credentials;
static uint8_t captured_salt[AIRDAP_SEC2_SALT_SIZE];
static uint8_t captured_verifier[AIRDAP_SEC2_VERIFIER_SIZE];

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return &identity;
}

esp_err_t airdap_sec2_credentials_load(airdap_sec2_credentials_t *credentials)
{
    assert(credentials != NULL);
    memset(credentials, 0, sizeof(*credentials));
    memset(credentials->salt, 0x11, sizeof(credentials->salt));
    memset(credentials->verifier, 0x22, sizeof(credentials->verifier));
    credentials->salt_len = sizeof(credentials->salt);
    credentials->verifier_len = sizeof(credentials->verifier);
    memset(credentials->fingerprint_hex, 'A',
        AIRDAP_SEC2_FINGERPRINT_HEX_LENGTH);
    credentials->fingerprint_hex[AIRDAP_SEC2_FINGERPRINT_HEX_LENGTH] = '\0';
    ++credential_load_count;
    return ESP_OK;
}

void airdap_sec2_credentials_clear(airdap_sec2_credentials_t *credentials)
{
    assert(credentials != NULL);
    memset(credentials, 0, sizeof(*credentials));
}

airdap_mode_state_result_t airdap_mode_state_transition(
    airdap_mode_event_t event)
{
    assert(mode_event_count < sizeof(mode_events) / sizeof(mode_events[0]));
    mode_events[mode_event_count++] = event;
    return AIRDAP_MODE_STATE_OK;
}

esp_err_t airdap_wifi_manager_prepare_provisioning(void)
{
    ++prepare_count;
    prepared_after_manager_init = prepare_result == ESP_OK;
    return prepare_result;
}

esp_err_t airdap_wifi_manager_stage_provisioning_credentials(
    const airdap_wifi_credentials_t *credentials)
{
    assert(credentials != NULL);
    staged_credentials = *credentials;
    ++stage_count;
    return ESP_OK;
}

esp_err_t airdap_wifi_manager_accept_provisioned_credentials(
    const airdap_wifi_credentials_t *credentials)
{
    assert(credentials != NULL);
    assert(memcmp(credentials, &staged_credentials, sizeof(*credentials)) == 0);
    ++accept_count;
    return ESP_OK;
}

esp_err_t airdap_wifi_manager_finish_provisioning(void)
{
    ++finish_count;
    return ESP_OK;
}

esp_err_t airdap_wifi_manager_clear_network_configuration(void)
{
    ++clear_count;
    return ESP_OK;
}

esp_err_t network_prov_mgr_init(network_prov_mgr_config_t config)
{
    assert(config.scheme.marker == network_prov_scheme_ble.marker);
    assert(config.network_prov_wifi_conn_cfg.wifi_conn_attempts == 3U);
    prepared_after_manager_init = false;
    ++manager_init_count;
    return ESP_OK;
}

esp_err_t network_prov_mgr_deinit(void)
{
    ++manager_deinit_count;
    return ESP_OK;
}

esp_err_t network_prov_mgr_start_provisioning(
    network_prov_security_t security,
    const void *security_params,
    const char *service_name,
    const char *service_key)
{
    assert(security == NETWORK_PROV_SECURITY_2);
    assert(prepared_after_manager_init);
    prepared_after_manager_init = false;
    assert(service_name != NULL && strcmp(service_name, identity.device_id) == 0);
    assert(service_key == NULL);
    const network_prov_security2_params_t *params = security_params;
    assert(params != NULL);
    assert(params->salt_len == sizeof(captured_salt));
    assert(params->verifier_len == sizeof(captured_verifier));
    memcpy(captured_salt, params->salt, sizeof(captured_salt));
    memcpy(captured_verifier, params->verifier, sizeof(captured_verifier));
    ++manager_start_count;
    return ESP_OK;
}

void network_prov_mgr_stop_provisioning(void)
{
    ++manager_stop_count;
}

esp_err_t esp_timer_create(
    const esp_timer_create_args_t *args,
    esp_timer_handle_t *output)
{
    assert(args != NULL && output != NULL);
    timer.callback = args->callback;
    timer.argument = args->arg;
    *output = &timer;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t handle)
{
    assert(handle == &timer);
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t handle)
{
    assert(handle == &timer);
    return timer.active;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout_us)
{
    assert(handle == &timer);
    timer.active = true;
    timer.timeout_us = timeout_us;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t handle)
{
    assert(handle == &timer);
    timer.active = false;
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void *event_handler_arg,
    esp_event_handler_instance_t *instance)
{
    (void) event_base;
    (void) event_id;
    (void) event_handler;
    (void) event_handler_arg;
    assert(instance != NULL);
    *instance = instance;
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_instance_t instance)
{
    (void) event_base;
    (void) event_id;
    (void) instance;
    return ESP_OK;
}

esp_err_t esp_event_post(
    esp_event_base_t event_base,
    int32_t event_id,
    const void *event_data,
    size_t event_data_size,
    uint32_t ticks_to_wait)
{
    (void) event_base;
    (void) event_id;
    (void) event_data;
    (void) event_data_size;
    (void) ticks_to_wait;
    return event_post_result;
}

BaseType_t xTaskCreate(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    unsigned int priority,
    TaskHandle_t *handle)
{
    assert(task != NULL && name != NULL && argument == NULL && handle != NULL);
    assert(stack_depth == 3072U && priority == 4U);
    *handle = &fake_task;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    (void) ticks;
}

esp_err_t airdap_boot_key_get_pressed(bool *pressed)
{
    assert(pressed != NULL);
    *pressed = false;
    return ESP_OK;
}

void esp_restart(void)
{
    ++restart_count;
}

static wifi_sta_config_t make_station(const char *ssid, const char *password)
{
    wifi_sta_config_t station = {0};
    memcpy(station.ssid, ssid, strlen(ssid));
    memcpy(station.password, password, strlen(password));
    return station;
}

static void finish_window(void)
{
    airdap_ble_provisioning_test_network_event(NETWORK_PROV_END, NULL);
    assert(!airdap_ble_provisioning_test_window_active());
}

int main(void)
{
    assert(airdap_ble_provisioning_start() == ESP_OK);
    assert(manager_init_count == 0U && manager_start_count == 0U);

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(airdap_ble_provisioning_test_window_active());
    assert(credential_load_count == 1U && manager_init_count == 1U);
    assert(manager_start_count == 1U && prepare_count == 1U);
    assert(timer.active && timer.timeout_us == 120000000U);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_STARTED);
    for (size_t index = 0U; index < sizeof(captured_salt); ++index) {
        assert(captured_salt[index] == 0x11U);
    }
    for (size_t index = 0U; index < sizeof(captured_verifier); ++index) {
        assert(captured_verifier[index] == 0x22U);
    }

    wifi_sta_config_t station = make_station("Lab AP", "test password");
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_RECV,
        &station);
    assert(stage_count == 1U);
    assert(staged_credentials.ssid_length == 6U);
    assert(staged_credentials.password_length == 13U);
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_SUCCESS,
        NULL);
    assert(accept_count == 1U && manager_stop_count == 0U);
    assert(!timer.active);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_SUCCEEDED);
    finish_window();
    assert(manager_deinit_count == 1U && finish_count == 1U);

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    timer.active = false;
    event_post_result = ESP_FAIL;
    const unsigned stops_before_post_failure = manager_stop_count;
    timer.callback(timer.argument);
    assert(manager_stop_count == stops_before_post_failure);
    assert(timer.active && timer.timeout_us == 100000U);
    event_post_result = ESP_OK;
    airdap_ble_provisioning_test_timeout();
    assert(manager_stop_count == 1U);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_TIMED_OUT);
    finish_window();
    assert(finish_count == 2U);

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(manager_stop_count == 2U);
    finish_window();
    assert(finish_count == 3U);

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_OK);
    assert(clear_count == 1U && restart_count == 0U);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_RESET);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_RELEASED) == ESP_OK);
    finish_window();
    assert(finish_count == 4U && restart_count == 1U);

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_OK);
    assert(clear_count == 2U && restart_count == 1U);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_RESET);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_RELEASED) == ESP_OK);
    assert(restart_count == 1U);
    finish_window();
    assert(finish_count == 5U && restart_count == 2U);

    prepare_result = ESP_FAIL;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_FAIL);
    assert(!airdap_ble_provisioning_test_window_active());
    assert(manager_init_count == 6U && manager_start_count == 5U);
    assert(manager_deinit_count == 6U && finish_count == 5U);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_OK);
    assert(clear_count == 3U && restart_count == 2U);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_RELEASED) == ESP_OK);
    assert(restart_count == 3U);

    prepare_result = ESP_OK;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    station = make_station("cancelled AP", "cancelled password");
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_RECV,
        &station);
    const unsigned accepts_before_cancel = accept_count;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_SUCCESS,
        NULL);
    assert(accept_count == accepts_before_cancel);
    finish_window();

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    station = make_station("cleared AP", "cleared password");
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_RECV,
        &station);
    const unsigned accepts_before_clear = accept_count;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_OK);
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_SUCCESS,
        NULL);
    assert(accept_count == accepts_before_clear);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_RELEASED) == ESP_OK);
    finish_window();

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    station = make_station("successful AP", "successful password");
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_RECV,
        &station);
    airdap_ble_provisioning_test_network_event(
        NETWORK_PROV_WIFI_CRED_SUCCESS,
        NULL);
    const unsigned stops_after_success = manager_stop_count;
    airdap_ble_provisioning_test_timeout();
    assert(manager_stop_count == stops_after_success);
    assert(airdap_ble_provisioning_test_window_active());
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_SUCCEEDED);
    finish_window();

    puts("BLE provisioning adapter tests passed");
    return 0;
}
