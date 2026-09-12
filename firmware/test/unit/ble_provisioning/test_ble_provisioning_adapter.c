#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airdap_ble_provisioning.h"
#include "airdap_ble_provisioning_internal.h"
#include "airdap_button_config.h"
#include "airdap_button_device_commands.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#include "airdap_network_auth.h"
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
extern esp_event_base_t AIRDAP_PROVISIONING_INTERNAL_EVENT;
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
static unsigned endpoint_create_count;
static unsigned endpoint_register_count;
static unsigned prepare_count;
static unsigned stage_count;
static unsigned accept_count;
static unsigned finish_count;
static unsigned clear_count;
static unsigned restart_count;
static unsigned credential_load_count;
static unsigned led_change_count;
static bool status_led_on;
static bool network_led_on;
static esp_err_t prepare_result = ESP_OK;
static esp_err_t clear_result = ESP_OK;
static esp_err_t led_result = ESP_OK;
static esp_err_t event_post_result = ESP_OK;
static bool prepared_after_manager_init;
static airdap_mode_event_t mode_events[32];
static size_t mode_event_count;
static airdap_wifi_credentials_t staged_credentials;
static uint8_t captured_salt[AIRDAP_SEC2_SALT_SIZE];
static uint8_t captured_verifier[AIRDAP_SEC2_VERIFIER_SIZE];
static protocomm_req_handler_t pairing_handler;
static unsigned pair_count;
static bool pairing_window_active;
static bool manager_active;
static bool endpoint_created_for_window;
static bool provisioning_started_for_window;
static TaskFunction_t button_task_function;
static jmp_buf task_done;
static unsigned task_tick;
static unsigned task_scenario;
static bool test_restart_binding;
static unsigned button_action_counts[9];
extern airdap_mode_dap_result_t fake_dap_route_result;
static airdap_button_command_t last_device_command;
static bool device_command_busy;
bool airdap_button_device_command_busy(void) { return device_command_busy; }
esp_err_t airdap_button_device_command_execute(airdap_button_command_t command)
{
    last_device_command = command;
    return ESP_OK;
}

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return &identity;
}

esp_err_t airdap_network_auth_set_pairing_window_active(bool active)
{
    pairing_window_active = active;
    return ESP_OK;
}

airdap_network_auth_result_t airdap_network_auth_pair(
    const uint8_t *request,
    size_t request_size,
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE])
{
    assert(pairing_window_active);
    assert(request != NULL && fingerprint != NULL);
    assert(request_size == AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE);
    assert(request[0] == AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION);
    for (size_t index = 1U; index < request_size; ++index) {
        assert(request[index] == (uint8_t) index);
    }
    memset(fingerprint, 0x5A, AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE);
    ++pair_count;
    return AIRDAP_NETWORK_AUTH_OK;
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
    assert(!pairing_window_active);
    ++clear_count;
    return clear_result;
}

esp_err_t network_prov_mgr_init(network_prov_mgr_config_t config)
{
    assert(config.scheme.marker == network_prov_scheme_ble.marker);
    assert(config.network_prov_wifi_conn_cfg.wifi_conn_attempts == 3U);
    assert(!manager_active);
    assert(!pairing_window_active);
    manager_active = true;
    endpoint_created_for_window = false;
    provisioning_started_for_window = false;
    prepared_after_manager_init = false;
    ++manager_init_count;
    return ESP_OK;
}

esp_err_t network_prov_mgr_deinit(void)
{
    assert(manager_active);
    assert(!pairing_window_active);
    manager_active = false;
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
    assert(manager_active && endpoint_created_for_window);
    provisioning_started_for_window = true;
    const network_prov_security2_params_t *params = security_params;
    assert(params != NULL);
    assert(params->salt_len == sizeof(captured_salt));
    assert(params->verifier_len == sizeof(captured_verifier));
    memcpy(captured_salt, params->salt, sizeof(captured_salt));
    memcpy(captured_verifier, params->verifier, sizeof(captured_verifier));
    ++manager_start_count;
    return ESP_OK;
}

esp_err_t network_prov_mgr_endpoint_create(const char *endpoint_name)
{
    assert(endpoint_name != NULL);
    assert(strcmp(endpoint_name, "airdap-pair") == 0);
    assert(manager_active && !endpoint_created_for_window &&
        !provisioning_started_for_window);
    endpoint_created_for_window = true;
    ++endpoint_create_count;
    return ESP_OK;
}

esp_err_t network_prov_mgr_endpoint_register(
    const char *endpoint_name,
    protocomm_req_handler_t handler,
    void *user_context)
{
    assert(endpoint_name != NULL);
    assert(strcmp(endpoint_name, "airdap-pair") == 0);
    assert(handler != NULL && user_context == NULL);
    assert(manager_active && endpoint_created_for_window &&
        provisioning_started_for_window);
    assert(pairing_window_active);
    pairing_handler = handler;
    ++endpoint_register_count;
    return ESP_OK;
}

void network_prov_mgr_endpoint_unregister(const char *endpoint_name)
{
    (void) endpoint_name;
}

void network_prov_mgr_stop_provisioning(void)
{
    assert(!pairing_window_active);
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
    if (event_base == AIRDAP_PROVISIONING_INTERNAL_EVENT &&
        event_id > 0 && event_id < 9) {
        assert(event_data != NULL && event_data_size == 1);
        const uint8_t command = *(const uint8_t *) event_data;
        if (event_id == AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK) assert(command == AIRDAP_BUTTON_COMMAND_NONE);
        if (event_id == AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK) {
            assert(command == (task_scenario == 3 ? (test_restart_binding ? AIRDAP_BUTTON_COMMAND_RESTART : AIRDAP_BUTTON_COMMAND_CLEAR_NETWORK_RESTART) : AIRDAP_BUTTON_COMMAND_DAP_TOGGLE));
            if (task_scenario == 3) assert(task_tick == 15);
        }
        if (event_id == AIRDAP_PROVISIONING_BUTTON_HOLD_6) assert(command == AIRDAP_BUTTON_COMMAND_NONE);
        if (event_id == AIRDAP_PROVISIONING_BUTTON_CLEAR) assert(command == AIRDAP_BUTTON_COMMAND_CLEAR_NETWORK_RESTART);
        ++button_action_counts[event_id];
    }
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
    button_task_function = task;
    *handle = &fake_task;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(20));
    bool expected_status = false;
    if (task_scenario == 0) {
        expected_status = task_tick >= 16 && task_tick < 21;
    } else if (task_scenario == 1 || task_scenario == 3) {
        expected_status = (task_tick >= 7 && task_tick < 12) ||
            (task_tick >= 17 && task_tick < 22);
    } else if (task_scenario == 4) {
        expected_status = (task_tick >= 7 && task_tick < 11) ||
            (task_tick >= 26 && task_tick < 31);
    } else if (task_scenario == 5 && task_tick >= 309) {
        expected_status = false;
    } else if (task_tick >= 99 && task_tick < 299) {
        expected_status = ((task_tick - 99) % 50) < 25;
    } else if (task_tick >= 299 && task_tick < 499) {
        expected_status = ((task_tick - 299) % 20) < 10;
    } else if (task_tick >= 499 && task_tick < 519) {
        expected_status = ((task_tick - 499) % 6) < 3;
    }
    /* The first threshold write fails; retry must succeed on the next tick. */
    if (task_scenario == 2 && task_tick == 99) expected_status = false;
    assert(status_led_on == expected_status);
    assert(!network_led_on);
    ++task_tick;
    if (task_tick == (task_scenario == 2 ? 530U : task_scenario == 5 ? 320U : 40U)) {
        longjmp(task_done, 1);
    }
}

esp_err_t airdap_boot_key_get_pressed(bool *pressed)
{
    assert(pressed != NULL);
    *pressed = task_scenario == 2 ? task_tick < 510 : task_scenario == 5 ? task_tick < 300 :
        task_tick < 2 || ((task_scenario == 1 || task_scenario == 3 || task_scenario == 4) && task_tick >= 4 && task_tick < 6) ||
        (task_scenario == 4 && task_tick >= 10 && task_tick < 12);
    if (task_scenario == 0 && task_tick == 1) {
        assert(airdap_button_config_set(AIRDAP_BUTTON_GESTURE_SINGLE, AIRDAP_BUTTON_COMMAND_PROVISIONING) == ESP_OK);
    }
    led_result = task_scenario == 2 && task_tick == 99 ? ESP_FAIL : ESP_OK;
    return ESP_OK;
}

esp_err_t airdap_board_leds_set(bool status_on, bool network_on)
{
    if (led_result == ESP_OK) {
        status_led_on = status_on;
        network_led_on = network_on;
    }
    ++led_change_count;
    return led_result;
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

static void test_route_commands(void)
{
    assert(airdap_mode_state_get_dap_route() == AIRDAP_DAP_ROUTE_NETWORK);
    assert(airdap_ble_provisioning_test_button_action(AIRDAP_PROVISIONING_BUTTON_HOLD_6) == ESP_OK);
    assert(airdap_mode_state_get_dap_route() == AIRDAP_DAP_ROUTE_NETWORK);
    const airdap_button_command_t route_commands[] = {
        AIRDAP_BUTTON_COMMAND_DAP_USB, AIRDAP_BUTTON_COMMAND_DAP_NETWORK, AIRDAP_BUTTON_COMMAND_DAP_AUTO};
    const airdap_dap_route_t expected_routes[] = {
        AIRDAP_DAP_ROUTE_USB, AIRDAP_DAP_ROUTE_NETWORK, AIRDAP_DAP_ROUTE_AUTO};
    for (unsigned i = 0; i < 3; ++i) {
        assert(airdap_button_config_set(AIRDAP_BUTTON_GESTURE_HOLD6, route_commands[i]) == ESP_OK);
        assert(airdap_ble_provisioning_test_button_action(AIRDAP_PROVISIONING_BUTTON_HOLD_6) == ESP_OK);
        assert(airdap_mode_state_get_dap_route() == expected_routes[i]);
    }
    fake_dap_route_result = AIRDAP_MODE_DAP_BUSY;
    assert(airdap_ble_provisioning_test_button_action(AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK) == ESP_ERR_INVALID_STATE);
    assert(airdap_mode_state_get_dap_route() == AIRDAP_DAP_ROUTE_AUTO);
    fake_dap_route_result = AIRDAP_MODE_DAP_ALLOWED;
    assert(airdap_button_config_defaults() == ESP_OK);
}

static void test_device_bindings(void)
{
    for (int c = AIRDAP_BUTTON_COMMAND_RESTART; c < AIRDAP_BUTTON_COMMAND_COUNT; ++c) {
        assert(airdap_button_config_set(AIRDAP_BUTTON_GESTURE_HOLD6, (airdap_button_command_t) c) == ESP_OK);
        last_device_command = AIRDAP_BUTTON_COMMAND_NONE;
        assert(airdap_ble_provisioning_test_button_action(AIRDAP_PROVISIONING_BUTTON_HOLD_6) == ESP_OK);
        assert(last_device_command == (airdap_button_command_t) c);
        device_command_busy = true;
        assert(airdap_ble_provisioning_test_button_action(AIRDAP_PROVISIONING_BUTTON_HOLD_6) == ESP_ERR_INVALID_STATE);
        device_command_busy = false;
    }
    assert(airdap_button_config_defaults() == ESP_OK);
}

int main(void)
{
    assert(airdap_ble_provisioning_start() == ESP_OK);
    assert(manager_init_count == 0U && manager_start_count == 0U);

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK) == ESP_OK);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK) == ESP_OK);
    assert(manager_init_count == 0U && manager_start_count == 0U);
    assert(clear_count == 0U && restart_count == 0U && led_change_count == 0U);
    test_route_commands();
    test_device_bindings();
    assert(!airdap_ble_provisioning_test_window_active());

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY) == ESP_OK);
    assert(led_change_count == 0U && !status_led_on && !network_led_on);
    assert(manager_init_count == 0U && manager_start_count == 0U);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(airdap_button_config_set(AIRDAP_BUTTON_GESTURE_HOLD6, AIRDAP_BUTTON_COMMAND_WIFI_TOGGLE) == ESP_OK);
    assert(airdap_ble_provisioning_test_button_action(AIRDAP_PROVISIONING_BUTTON_HOLD_6) == ESP_ERR_INVALID_STATE);
    assert(airdap_button_config_defaults() == ESP_OK);
    assert(led_change_count == 0U && !status_led_on && !network_led_on);
    assert(airdap_ble_provisioning_test_window_active());
    assert(credential_load_count == 1U && manager_init_count == 1U);
    assert(manager_start_count == 1U && prepare_count == 1U);
    assert(endpoint_create_count == 1U && endpoint_register_count == 1U);
    assert(pairing_window_active);
    assert(timer.active && timer.timeout_us == 120000000U);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_STARTED);
    for (size_t index = 0U; index < sizeof(captured_salt); ++index) {
        assert(captured_salt[index] == 0x11U);
    }
    for (size_t index = 0U; index < sizeof(captured_verifier); ++index) {
        assert(captured_verifier[index] == 0x22U);
    }

    uint8_t pair_request[AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE] = {
        AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION,
    };
    for (size_t index = 1U; index < sizeof(pair_request); ++index) {
        pair_request[index] = (uint8_t) index;
    }
    uint8_t *pair_response = NULL;
    ssize_t pair_response_size = 0;
    assert(pairing_handler(
        7U,
        pair_request,
        sizeof(pair_request),
        &pair_response,
        &pair_response_size,
        NULL) == ESP_OK);
    assert(pair_count == 1U);
    assert(pair_response != NULL);
    assert(pair_response_size == AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE);
    for (ssize_t index = 0; index < pair_response_size; ++index) {
        assert(pair_response[index] == 0x5AU);
    }
    free(pair_response);
    pair_response = NULL;
    pair_response_size = 0;
    assert(pairing_handler(
        7U,
        pair_request,
        sizeof(pair_request) - 1U,
        &pair_response,
        &pair_response_size,
        NULL) == ESP_ERR_INVALID_ARG);
    assert(pair_count == 1U && pair_response == NULL && pair_response_size == 0);

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
    const unsigned clears_before_ready = clear_count;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR_READY) == ESP_OK);
    assert(!status_led_on && !network_led_on);
    assert(clear_count == clears_before_ready);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_OK);
    assert(!status_led_on && !network_led_on);
    assert(clear_count == 1U && restart_count == 0U);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_RESET);
    finish_window();
    assert(finish_count == 4U && restart_count == 1U);

    const unsigned credential_loads_after_clear = credential_load_count;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    assert(credential_load_count == credential_loads_after_clear + 1U);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_OK);
    assert(clear_count == 2U && restart_count == 1U);
    assert(mode_events[mode_event_count - 1U] ==
        AIRDAP_MODE_EVENT_PROVISIONING_RESET);
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
    assert(clear_count == 3U && restart_count == 3U);

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

    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_TOGGLE) == ESP_OK);
    clear_result = ESP_FAIL;
    const unsigned stops_before_clear_failure = manager_stop_count;
    const unsigned restarts_before_clear_failure = restart_count;
    const size_t mode_events_before_clear_failure = mode_event_count;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_FAIL);
    assert(manager_stop_count == stops_before_clear_failure + 1U);
    assert(mode_event_count == mode_events_before_clear_failure);
    finish_window();
    assert(restart_count == restarts_before_clear_failure);

    clear_result = ESP_OK;
    led_result = ESP_FAIL;
    const unsigned clears_before_indicator_failure = clear_count;
    const unsigned restarts_before_indicator_failure = restart_count;
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR_READY) == ESP_OK);
    assert(clear_count == clears_before_indicator_failure);
    assert(airdap_ble_provisioning_test_button_action(
        AIRDAP_PROVISIONING_BUTTON_CLEAR) == ESP_OK);
    assert(clear_count == clears_before_indicator_failure + 1U);
    assert(restart_count == restarts_before_indicator_failure + 1U);

    /* Run the real polling task against sampled button inputs and GPIO output.
     * Event actions remain separate; failed LED writes must not suppress them. */
    memset(button_action_counts, 0, sizeof(button_action_counts));
    for (task_scenario = 0; task_scenario < 6; ++task_scenario) {
        assert(airdap_button_config_defaults() == ESP_OK);
        if (task_scenario == 3 || task_scenario == 4) {
            assert(airdap_button_config_set(AIRDAP_BUTTON_GESTURE_DOUBLE, AIRDAP_BUTTON_COMMAND_CLEAR_NETWORK_RESTART) == ESP_OK);
        }
        task_tick = 0;
        if (setjmp(task_done) == 0) button_task_function(NULL);
    }
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK] == 2);
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK] == 2);
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_TOGGLE_READY] == 2);
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_CLEAR_READY] == 1);
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_HOLD_6_READY] == 2);
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_HOLD_6] == 1);
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_CLEAR] == 1);
    assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_TOGGLE] == 0);
    test_restart_binding = true;
    for (task_scenario = 3; task_scenario <= 4; ++task_scenario) {
        memset(button_action_counts, 0, sizeof(button_action_counts));
        assert(airdap_button_config_defaults() == ESP_OK);
        assert(airdap_button_config_set(AIRDAP_BUTTON_GESTURE_DOUBLE, AIRDAP_BUTTON_COMMAND_RESTART) == ESP_OK);
        task_tick = 0;
        if (setjmp(task_done) == 0) button_task_function(NULL);
        assert(button_action_counts[AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK] == (task_scenario == 3 ? 1U : 0U));
    }
    puts("BLE provisioning adapter tests passed");
    return 0;
}
