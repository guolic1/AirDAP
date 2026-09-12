#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airdap_ble_provisioning.h"
#include "airdap_ble_provisioning_internal.h"
#include "airdap_board.h"
#include "airdap_button_config.h"
#include "airdap_button_indicator.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#include "airdap_network_auth.h"
#include "airdap_provisioning_button.h"
#include "airdap_sec2_credentials.h"
#include "airdap_wifi_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

ESP_EVENT_DEFINE_BASE(AIRDAP_PROVISIONING_INTERNAL_EVENT);

enum {
    BUTTON_POLL_MS = 20,
    PROVISIONING_WINDOW_US = 120000000,
    BUTTON_TASK_STACK_SIZE = 3072,
    BUTTON_TASK_PRIORITY = 4,
    PROVISIONING_WIFI_ATTEMPTS = 3,
    INTERNAL_EVENT_TIMEOUT = 100,
    TIMEOUT_RETRY_US = 100000,
};

static const char *PAIRING_ENDPOINT = "airdap-pair";

typedef enum {
    WINDOW_OUTCOME_NONE = 0,
    WINDOW_OUTCOME_SUCCESS,
    WINDOW_OUTCOME_RESTORE,
    WINDOW_OUTCOME_CLEAR,
    WINDOW_OUTCOME_CLEAR_FAILED,
} window_outcome_t;

static const char *TAG = "airdap_prov";
static esp_timer_handle_t window_timer;
static TaskHandle_t button_task_handle;
static esp_event_handler_instance_t internal_event_instance;
static esp_event_handler_instance_t network_event_instance;
static airdap_sec2_credentials_t security2_credentials;
static network_prov_security2_params_t security2_params;
static airdap_wifi_credentials_t pending_wifi_credentials;
static bool monitor_started;
static bool manager_initialized;
static bool wifi_manager_suspended;
static bool window_active;
static bool stop_requested;
static bool pending_wifi_credentials_valid;
static bool restart_pending;
static window_outcome_t window_outcome;

static void clear_bytes(void *data, size_t size)
{
    volatile uint8_t *byte = (volatile uint8_t *) data;
    for (size_t index = 0U; index < size; ++index) {
        byte[index] = 0U;
    }
}

static size_t bounded_length(const uint8_t *value, size_t capacity)
{
    size_t length = 0U;
    while (length < capacity && value[length] != 0U) {
        ++length;
    }
    return length;
}

static void clear_pending_credentials(void)
{
    clear_bytes(&pending_wifi_credentials, sizeof(pending_wifi_credentials));
    pending_wifi_credentials_valid = false;
}

static esp_err_t set_pairing_window_active(bool active)
{
    const esp_err_t error =
        airdap_network_auth_set_pairing_window_active(active);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Network pairing window update failed: %s",
            esp_err_to_name(error));
    }
    return error;
}

static esp_err_t pairing_endpoint_handler(
    uint32_t session_id,
    const uint8_t *input,
    ssize_t input_length,
    uint8_t **output,
    ssize_t *output_length,
    void *private_data)
{
    (void) session_id;
    (void) private_data;
    if (output == NULL || output_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *output = NULL;
    *output_length = 0;
    if (input == NULL || input_length !=
        AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE];
    const airdap_network_auth_result_t result = airdap_network_auth_pair(
        input,
        (size_t) input_length,
        fingerprint);
    if (result != AIRDAP_NETWORK_AUTH_OK) {
        clear_bytes(fingerprint, sizeof(fingerprint));
        return result == AIRDAP_NETWORK_AUTH_UNSUPPORTED_VERSION
            ? ESP_ERR_INVALID_VERSION
            : result == AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT
                ? ESP_ERR_INVALID_ARG
                : ESP_FAIL;
    }

    uint8_t *response = malloc(sizeof(fingerprint));
    if (response == NULL) {
        clear_bytes(fingerprint, sizeof(fingerprint));
        return ESP_ERR_NO_MEM;
    }
    memcpy(response, fingerprint, sizeof(fingerprint));
    *output = response;
    *output_length = sizeof(fingerprint);

    char fingerprint_hex[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE * 2U + 1U];
    for (size_t index = 0U; index < sizeof(fingerprint); ++index) {
        (void) snprintf(
            fingerprint_hex + index * 2U,
            sizeof(fingerprint_hex) - index * 2U,
            "%02x",
            fingerprint[index]);
    }
    ESP_LOGI(TAG, "Network credential committed; fingerprint=%s",
        fingerprint_hex);
    clear_bytes(fingerprint_hex, sizeof(fingerprint_hex));
    clear_bytes(fingerprint, sizeof(fingerprint));
    return ESP_OK;
}

static void publish_mode(airdap_mode_event_t event)
{
    const airdap_mode_state_result_t result =
        airdap_mode_state_transition(event);
    if (result != AIRDAP_MODE_STATE_OK) {
        ESP_LOGE(TAG, "Provisioning mode transition failed: %d", result);
    }
}

static void stop_window_timer(void)
{
    if (window_timer != NULL && esp_timer_is_active(window_timer)) {
        const esp_err_t error = esp_timer_stop(window_timer);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning timer stop failed: %s",
                esp_err_to_name(error));
        }
    }
}

static void maybe_restart_after_clear(void)
{
    if (restart_pending && !window_active) {
        restart_pending = false;
        esp_restart();
    }
}

static void request_window_stop(window_outcome_t outcome)
{
    if (!window_active) {
        return;
    }
    if (window_outcome == WINDOW_OUTCOME_SUCCESS &&
        outcome == WINDOW_OUTCOME_RESTORE) {
        return;
    }
    (void) set_pairing_window_active(false);
    if (stop_requested) {
        if (outcome == WINDOW_OUTCOME_CLEAR &&
            window_outcome != WINDOW_OUTCOME_CLEAR) {
            window_outcome = WINDOW_OUTCOME_CLEAR;
            publish_mode(AIRDAP_MODE_EVENT_PROVISIONING_RESET);
        }
        return;
    }
    stop_requested = true;
    window_outcome = outcome;
    stop_window_timer();
    if (outcome == WINDOW_OUTCOME_RESTORE) {
        publish_mode(AIRDAP_MODE_EVENT_PROVISIONING_TIMED_OUT);
    } else if (outcome == WINDOW_OUTCOME_CLEAR) {
        publish_mode(AIRDAP_MODE_EVENT_PROVISIONING_RESET);
    }
    network_prov_mgr_stop_provisioning();
}

static void cleanup_failed_window_start(void)
{
    (void) set_pairing_window_active(false);
    if (manager_initialized) {
        const esp_err_t error = network_prov_mgr_deinit();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning manager cleanup failed: %s",
                esp_err_to_name(error));
        }
        manager_initialized = false;
    }
    if (wifi_manager_suspended) {
        const esp_err_t error = airdap_wifi_manager_finish_provisioning();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi controller resume failed: %s",
                esp_err_to_name(error));
        }
        wifi_manager_suspended = false;
    }
    airdap_sec2_credentials_clear(&security2_credentials);
    clear_bytes(&security2_params, sizeof(security2_params));
    clear_pending_credentials();
    window_active = false;
    stop_requested = false;
    window_outcome = WINDOW_OUTCOME_NONE;
}

static esp_err_t start_window(void)
{
    const airdap_device_identity_t *identity = airdap_device_identity_get();
    if (identity == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = airdap_sec2_credentials_load(&security2_credentials);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Security 2 credentials unavailable: %s",
            esp_err_to_name(error));
        return error;
    }

    const network_prov_mgr_config_t configuration = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = PROVISIONING_WIFI_ATTEMPTS,
        },
    };
    error = network_prov_mgr_init(configuration);
    if (error != ESP_OK) {
        cleanup_failed_window_start();
        return error;
    }
    manager_initialized = true;

    error = network_prov_mgr_endpoint_create(PAIRING_ENDPOINT);
    if (error != ESP_OK) {
        cleanup_failed_window_start();
        return error;
    }

    error = airdap_wifi_manager_prepare_provisioning();
    if (error != ESP_OK) {
        cleanup_failed_window_start();
        return error;
    }
    wifi_manager_suspended = true;

    security2_params.salt = (const char *) security2_credentials.salt;
    security2_params.salt_len = security2_credentials.salt_len;
    security2_params.verifier =
        (const char *) security2_credentials.verifier;
    security2_params.verifier_len = security2_credentials.verifier_len;
    error = network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_2,
        &security2_params,
        identity->device_id,
        NULL);
    if (error != ESP_OK) {
        cleanup_failed_window_start();
        return error;
    }
    error = set_pairing_window_active(true);
    if (error != ESP_OK) {
        network_prov_mgr_stop_provisioning();
        cleanup_failed_window_start();
        return error;
    }
    error = network_prov_mgr_endpoint_register(
        PAIRING_ENDPOINT,
        pairing_endpoint_handler,
        NULL);
    if (error != ESP_OK) {
        (void) set_pairing_window_active(false);
        network_prov_mgr_stop_provisioning();
        cleanup_failed_window_start();
        return error;
    }
    clear_pending_credentials();
    window_active = true;
    stop_requested = false;
    window_outcome = WINDOW_OUTCOME_NONE;
    publish_mode(AIRDAP_MODE_EVENT_PROVISIONING_STARTED);

    error = esp_timer_start_once(window_timer, PROVISIONING_WINDOW_US);
    if (error != ESP_OK) {
        request_window_stop(WINDOW_OUTCOME_RESTORE);
        return error;
    }
    ESP_LOGI(TAG, "BLE Security 2 window started; credential fingerprint=%s",
        security2_credentials.fingerprint_hex);
    return ESP_OK;
}

static esp_err_t capture_candidate_credentials(const wifi_sta_config_t *station)
{
    if (station == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ssid_length = bounded_length(
        station->ssid,
        sizeof(station->ssid));
    const size_t password_length = bounded_length(
        station->password,
        sizeof(station->password));
    if (ssid_length == 0U || ssid_length > AIRDAP_WIFI_SSID_MAX_LENGTH ||
        password_length > AIRDAP_WIFI_PASSWORD_MAX_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    clear_pending_credentials();
    memcpy(pending_wifi_credentials.ssid, station->ssid, ssid_length);
    memcpy(
        pending_wifi_credentials.password,
        station->password,
        password_length);
    pending_wifi_credentials.ssid_length = (uint8_t) ssid_length;
    pending_wifi_credentials.password_length = (uint8_t) password_length;
    const esp_err_t error =
        airdap_wifi_manager_stage_provisioning_credentials(
            &pending_wifi_credentials);
    pending_wifi_credentials_valid = error == ESP_OK;
    if (error != ESP_OK) {
        clear_pending_credentials();
    }
    return error;
}

static void handle_window_end(void)
{
    stop_window_timer();
    (void) set_pairing_window_active(false);
    if (manager_initialized) {
        const esp_err_t error = network_prov_mgr_deinit();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning manager deinit failed: %s",
                esp_err_to_name(error));
        }
        manager_initialized = false;
    }
    window_active = false;
    stop_requested = false;
    airdap_sec2_credentials_clear(&security2_credentials);
    clear_bytes(&security2_params, sizeof(security2_params));
    clear_pending_credentials();

    if (wifi_manager_suspended) {
        const esp_err_t error = airdap_wifi_manager_finish_provisioning();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi controller resume failed: %s",
                esp_err_to_name(error));
        }
        wifi_manager_suspended = false;
    }
    maybe_restart_after_clear();
}

static void handle_network_event(int32_t event_id, void *event_data)
{
    if (!window_active) {
        return;
    }
    switch (event_id) {
    case NETWORK_PROV_WIFI_CRED_RECV: {
        if (stop_requested || window_outcome != WINDOW_OUTCOME_NONE) {
            break;
        }
        const esp_err_t error = capture_candidate_credentials(event_data);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning credential staging failed: %s",
                esp_err_to_name(error));
            request_window_stop(WINDOW_OUTCOME_RESTORE);
        }
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL:
        clear_pending_credentials();
        ESP_LOGW(TAG, "Provisioning Wi-Fi verification failed");
        break;
    case NETWORK_PROV_WIFI_CRED_SUCCESS: {
        if (stop_requested || window_outcome != WINDOW_OUTCOME_NONE) {
            clear_pending_credentials();
            break;
        }
        const esp_err_t error = pending_wifi_credentials_valid
            ? airdap_wifi_manager_accept_provisioned_credentials(
                &pending_wifi_credentials)
            : ESP_ERR_INVALID_STATE;
        clear_pending_credentials();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning commit failed: %s",
                esp_err_to_name(error));
            request_window_stop(WINDOW_OUTCOME_RESTORE);
            break;
        }
        window_outcome = WINDOW_OUTCOME_SUCCESS;
        publish_mode(AIRDAP_MODE_EVENT_PROVISIONING_SUCCEEDED);
        stop_window_timer();
        break;
    }
    case NETWORK_PROV_END:
        handle_window_end();
        break;
    default:
        break;
    }
}

static esp_err_t execute_button_command(airdap_button_command_t command)
{
    switch (command) {
    case AIRDAP_BUTTON_COMMAND_NONE:
        return ESP_OK;
    case AIRDAP_BUTTON_COMMAND_DAP_TOGGLE:
    case AIRDAP_BUTTON_COMMAND_DAP_USB:
    case AIRDAP_BUTTON_COMMAND_DAP_NETWORK:
    case AIRDAP_BUTTON_COMMAND_DAP_AUTO: {
        const airdap_dap_route_t route = command == AIRDAP_BUTTON_COMMAND_DAP_TOGGLE
            ? AIRDAP_DAP_ROUTE_TOGGLE : command == AIRDAP_BUTTON_COMMAND_DAP_USB
            ? AIRDAP_DAP_ROUTE_USB : command == AIRDAP_BUTTON_COMMAND_DAP_NETWORK
            ? AIRDAP_DAP_ROUTE_NETWORK : AIRDAP_DAP_ROUTE_AUTO;
        const airdap_mode_dap_result_t result = airdap_mode_state_set_dap_route(route);
        if (result != AIRDAP_MODE_DAP_ALLOWED) {
            ESP_LOGW(TAG, "BOOT_KEY DAP selection rejected: %d", result);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "DAP route=%d (auto=0 usb=1 network=2)", airdap_mode_state_get_dap_route());
        return ESP_OK;
    }
    case AIRDAP_BUTTON_COMMAND_PROVISIONING:
        if (window_active) {
            request_window_stop(window_outcome == WINDOW_OUTCOME_SUCCESS
                ? WINDOW_OUTCOME_SUCCESS
                : WINDOW_OUTCOME_RESTORE);
            return ESP_OK;
        }
        return start_window();
    case AIRDAP_BUTTON_COMMAND_CLEAR_NETWORK_RESTART: {
        const esp_err_t pairing_error = set_pairing_window_active(false);
        if (pairing_error != ESP_OK) {
            request_window_stop(WINDOW_OUTCOME_CLEAR_FAILED);
            return pairing_error;
        }
        const esp_err_t error =
            airdap_wifi_manager_clear_network_configuration();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Network configuration clear failed: %s",
                esp_err_to_name(error));
            request_window_stop(WINDOW_OUTCOME_CLEAR_FAILED);
            return error;
        }
        restart_pending = true;
        if (window_active) {
            request_window_stop(WINDOW_OUTCOME_CLEAR);
        } else {
            publish_mode(AIRDAP_MODE_EVENT_PROVISIONING_RESET);
        }
        maybe_restart_after_clear();
        return ESP_OK;
    }
    case AIRDAP_BUTTON_COMMAND_COUNT:
        break;
    }
    return ESP_ERR_INVALID_ARG;
}

static int gesture_for_action(airdap_provisioning_button_action_t action)
{
    switch (action) {
    case AIRDAP_PROVISIONING_BUTTON_SINGLE_CLICK: return AIRDAP_BUTTON_GESTURE_SINGLE;
    case AIRDAP_PROVISIONING_BUTTON_DOUBLE_CLICK: return AIRDAP_BUTTON_GESTURE_DOUBLE;
    case AIRDAP_PROVISIONING_BUTTON_TOGGLE: return AIRDAP_BUTTON_GESTURE_HOLD2;
    case AIRDAP_PROVISIONING_BUTTON_HOLD_6: return AIRDAP_BUTTON_GESTURE_HOLD6;
    case AIRDAP_PROVISIONING_BUTTON_CLEAR: return AIRDAP_BUTTON_GESTURE_HOLD10;
    default: return -1;
    }
}

static void provisioning_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void) argument;
    if (event_base == NETWORK_PROV_EVENT) {
        handle_network_event(event_id, event_data);
    } else if (event_base == AIRDAP_PROVISIONING_INTERNAL_EVENT) {
        if (event_id == INTERNAL_EVENT_TIMEOUT) {
            request_window_stop(WINDOW_OUTCOME_RESTORE);
        } else {
            const int gesture = gesture_for_action((airdap_provisioning_button_action_t) event_id);
            if (gesture >= 0 && event_data != NULL) {
                const airdap_button_command_t command = *(const uint8_t *) event_data;
                const esp_err_t error = execute_button_command(command);
                if (error != ESP_OK) {
                    ESP_LOGE(TAG, "BOOT_KEY command failed: %s", esp_err_to_name(error));
                }
                ESP_LOGI(TAG, "BOOT_KEY %s command=%s result=%s",
                    airdap_button_gesture_name((airdap_button_gesture_t) gesture),
                    airdap_button_command_name(command), esp_err_to_name(error));
            }
        }
    }
}

static void window_timer_callback(void *argument)
{
    (void) argument;
    const esp_err_t error = esp_event_post(
        AIRDAP_PROVISIONING_INTERNAL_EVENT,
        INTERNAL_EVENT_TIMEOUT,
        NULL,
        0U,
        0U);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning timeout event lost: %s",
            esp_err_to_name(error));
        const esp_err_t retry_error = esp_timer_start_once(
            window_timer,
            TIMEOUT_RETRY_US);
        if (retry_error != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning timeout retry failed: %s",
                esp_err_to_name(retry_error));
        }
    }
}

static void button_task(void *argument)
{
    (void) argument;
    airdap_provisioning_button_t button;
    airdap_button_indicator_t indicator = {0};
    bool indicator_valid = false;
    bool last_status_on = false;
    uint8_t commands[AIRDAP_BUTTON_GESTURE_COUNT] = {0};
    uint32_t stable_release_ms = 0U;
    airdap_provisioning_button_action_t pending_clear = AIRDAP_PROVISIONING_BUTTON_NONE;
    airdap_provisioning_button_init(&button);
    for (;;) {
        airdap_provisioning_button_action_t action = AIRDAP_PROVISIONING_BUTTON_NONE;
        bool pressed = false;
        const esp_err_t error = airdap_boot_key_get_pressed(&pressed);
        if (error != ESP_OK || pressed) {
            stable_release_ms = 0U;
            if (pending_clear != AIRDAP_PROVISIONING_BUTTON_NONE) {
                ESP_LOGW(TAG, "BOOT_KEY clear cancelled before stable release");
                pending_clear = AIRDAP_PROVISIONING_BUTTON_NONE;
            }
        } else if (stable_release_ms < 200U) {
            stable_release_ms += BUTTON_POLL_MS;
        }
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "BOOT_KEY read failed: %s", esp_err_to_name(error));
        } else {
            /* A binding edit cannot change the meaning of an already-held key. */
            if (button.state == AIRDAP_BUTTON_IDLE && pressed) {
                const esp_err_t config_error = airdap_button_config_get(commands);
                if (config_error != ESP_OK) {
                    ESP_LOGE(TAG, "BOOT_KEY config read failed: %s", esp_err_to_name(config_error));
                    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                    continue;
                }
            }
            action = airdap_provisioning_button_step(
                &button,
                pressed,
                BUTTON_POLL_MS);
        }
        airdap_button_indicator_step(&indicator, &button, action, BUTTON_POLL_MS);
        /* One task owns both LED outputs; event-loop actions cannot race the
         * pattern or leave the old provisioning indication latched. */
        if (!indicator_valid || last_status_on != indicator.status_on) {
            const esp_err_t led_error = airdap_board_leds_set(indicator.status_on, false);
            indicator_valid = led_error == ESP_OK;
            if (indicator_valid) {
                last_status_on = indicator.status_on;
            } else {
                ESP_LOGE(TAG, "BOOT_KEY indicator update failed: %s",
                    esp_err_to_name(led_error));
            }
        }
        const int recognized_gesture = gesture_for_action(action);
        if (recognized_gesture >= 0 &&
            commands[recognized_gesture] == AIRDAP_BUTTON_COMMAND_CLEAR_NETWORK_RESTART &&
            stable_release_ms < 200U) {
            pending_clear = action;
            action = AIRDAP_PROVISIONING_BUTTON_NONE;
        }
        if (pending_clear != AIRDAP_PROVISIONING_BUTTON_NONE && stable_release_ms >= 200U) {
            action = pending_clear;
            pending_clear = AIRDAP_PROVISIONING_BUTTON_NONE;
        }
        if (action != AIRDAP_PROVISIONING_BUTTON_NONE) {
            const int gesture = gesture_for_action(action);
            const uint8_t command = gesture >= 0 ? commands[gesture] : AIRDAP_BUTTON_COMMAND_NONE;
            const esp_err_t post_error = esp_event_post(
                AIRDAP_PROVISIONING_INTERNAL_EVENT,
                action,
                &command,
                sizeof(command),
                portMAX_DELAY);
            if (post_error != ESP_OK) {
                ESP_LOGE(TAG, "BOOT_KEY event lost: %s",
                    esp_err_to_name(post_error));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t airdap_ble_provisioning_start(void)
{
    if (monitor_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t config_error = airdap_button_config_init();
    if (config_error != ESP_OK) return config_error;
    const esp_timer_create_args_t timer_args = {
        .callback = window_timer_callback,
        .name = "airdap_prov_window",
    };
    esp_err_t error = esp_timer_create(&timer_args, &window_timer);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_handler_instance_register(
        AIRDAP_PROVISIONING_INTERNAL_EVENT,
        ESP_EVENT_ANY_ID,
        provisioning_event_handler,
        NULL,
        &internal_event_instance);
    if (error != ESP_OK) {
        (void) esp_timer_delete(window_timer);
        window_timer = NULL;
        return error;
    }
    error = esp_event_handler_instance_register(
        NETWORK_PROV_EVENT,
        ESP_EVENT_ANY_ID,
        provisioning_event_handler,
        NULL,
        &network_event_instance);
    if (error != ESP_OK) {
        (void) esp_event_handler_instance_unregister(
            AIRDAP_PROVISIONING_INTERNAL_EVENT,
            ESP_EVENT_ANY_ID,
            internal_event_instance);
        (void) esp_timer_delete(window_timer);
        window_timer = NULL;
        return error;
    }
    if (xTaskCreate(
            button_task,
            "airdap_prov_button",
            BUTTON_TASK_STACK_SIZE,
            NULL,
            BUTTON_TASK_PRIORITY,
            &button_task_handle) != pdPASS) {
        (void) esp_event_handler_instance_unregister(
            NETWORK_PROV_EVENT,
            ESP_EVENT_ANY_ID,
            network_event_instance);
        (void) esp_event_handler_instance_unregister(
            AIRDAP_PROVISIONING_INTERNAL_EVENT,
            ESP_EVENT_ANY_ID,
            internal_event_instance);
        (void) esp_timer_delete(window_timer);
        window_timer = NULL;
        return ESP_ERR_NO_MEM;
    }
    monitor_started = true;
    return ESP_OK;
}

#ifdef AIRDAP_BLE_PROVISIONING_TESTING
esp_err_t airdap_ble_provisioning_test_button_action(
    airdap_provisioning_button_action_t action)
{
    const int gesture = gesture_for_action(action);
    if (gesture < 0) return ESP_OK;
    uint8_t commands[AIRDAP_BUTTON_GESTURE_COUNT];
    const esp_err_t error = airdap_button_config_get(commands);
    return error == ESP_OK ? execute_button_command(commands[gesture]) : error;
}

void airdap_ble_provisioning_test_network_event(
    int32_t event_id,
    void *event_data)
{
    handle_network_event(event_id, event_data);
}

void airdap_ble_provisioning_test_timeout(void)
{
    request_window_stop(WINDOW_OUTCOME_RESTORE);
}

bool airdap_ble_provisioning_test_window_active(void)
{
    return window_active;
}
#endif
