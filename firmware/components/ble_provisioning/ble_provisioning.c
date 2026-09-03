#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_ble_provisioning.h"
#include "airdap_ble_provisioning_internal.h"
#include "airdap_board.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
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
    BUTTON_POLL_MS = 100,
    PROVISIONING_WINDOW_US = 120000000,
    BUTTON_TASK_STACK_SIZE = 3072,
    BUTTON_TASK_PRIORITY = 4,
    PROVISIONING_WIFI_ATTEMPTS = 3,
    INTERNAL_EVENT_TIMEOUT = 100,
    TIMEOUT_RETRY_US = BUTTON_POLL_MS * 1000,
};

typedef enum {
    WINDOW_OUTCOME_NONE = 0,
    WINDOW_OUTCOME_SUCCESS,
    WINDOW_OUTCOME_RESTORE,
    WINDOW_OUTCOME_CLEAR,
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
static bool restart_after_release;
static bool button_released;
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
    if (restart_after_release && button_released && !window_active) {
        restart_after_release = false;
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
    clear_pending_credentials();
    window_active = true;
    stop_requested = false;
    button_released = false;
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

static esp_err_t handle_button_action(
    airdap_provisioning_button_action_t action)
{
    switch (action) {
    case AIRDAP_PROVISIONING_BUTTON_TOGGLE:
        if (window_active) {
            request_window_stop(window_outcome == WINDOW_OUTCOME_SUCCESS
                ? WINDOW_OUTCOME_SUCCESS
                : WINDOW_OUTCOME_RESTORE);
            return ESP_OK;
        }
        return start_window();
    case AIRDAP_PROVISIONING_BUTTON_CLEAR: {
        const esp_err_t error =
            airdap_wifi_manager_clear_network_configuration();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Network configuration clear failed: %s",
                esp_err_to_name(error));
            return error;
        }
        restart_after_release = true;
        button_released = false;
        if (window_active) {
            request_window_stop(WINDOW_OUTCOME_CLEAR);
        } else {
            publish_mode(AIRDAP_MODE_EVENT_PROVISIONING_RESET);
        }
        maybe_restart_after_clear();
        return ESP_OK;
    }
    case AIRDAP_PROVISIONING_BUTTON_RELEASED:
        button_released = true;
        maybe_restart_after_clear();
        return ESP_OK;
    case AIRDAP_PROVISIONING_BUTTON_NONE:
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
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
            (void) handle_button_action(
                (airdap_provisioning_button_action_t) event_id);
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
    airdap_provisioning_button_init(&button);
    for (;;) {
        bool pressed = false;
        const esp_err_t error = airdap_boot_key_get_pressed(&pressed);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "BOOT_KEY read failed: %s", esp_err_to_name(error));
        } else {
            const airdap_provisioning_button_action_t action =
                airdap_provisioning_button_step(
                    &button,
                    pressed,
                    BUTTON_POLL_MS);
            if (action != AIRDAP_PROVISIONING_BUTTON_NONE) {
                const esp_err_t post_error = esp_event_post(
                    AIRDAP_PROVISIONING_INTERNAL_EVENT,
                    action,
                    NULL,
                    0U,
                    portMAX_DELAY);
                if (post_error != ESP_OK) {
                    ESP_LOGE(TAG, "BOOT_KEY event lost: %s",
                        esp_err_to_name(post_error));
                }
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
    return handle_button_action(action);
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
