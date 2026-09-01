#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_config_store.h"
#include "airdap_mode_state.h"
#include "airdap_wifi_credentials.h"
#include "airdap_wifi_disconnect_reason.h"
#include "airdap_wifi_manager.h"
#include "airdap_wifi_state_machine.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

ESP_EVENT_DEFINE_BASE(AIRDAP_WIFI_INTERNAL_EVENT);

typedef enum {
    AIRDAP_WIFI_INTERNAL_RETRY = 0,
    AIRDAP_WIFI_INTERNAL_CONFIGURATION_CHANGED,
} airdap_wifi_internal_event_t;

static const char *TAG = "airdap_wifi";

static airdap_wifi_state_machine_t state_machine;
static esp_netif_t *station_netif;
static esp_timer_handle_t retry_timer;
static esp_event_handler_instance_t wifi_event_instance;
static esp_event_handler_instance_t ip_event_instance;
static esp_event_handler_instance_t internal_event_instance;
static bool event_loop_created;
static bool wifi_initialized;
static bool started;
static bool link_active;
static bool connection_in_progress;
static bool reconfiguration_pending;

static void clear_bytes(void *data, size_t size)
{
    volatile uint8_t *byte = (volatile uint8_t *) data;
    for (size_t index = 0U; index < size; ++index) {
        byte[index] = 0U;
    }
}

static esp_err_t load_stored_credentials(
    airdap_wifi_credentials_t *credentials,
    bool *found)
{
    if (credentials == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t encoded[AIRDAP_WIFI_CREDENTIALS_ENCODED_MAX_SIZE];
    size_t encoded_size = sizeof(encoded);
    esp_err_t error = airdap_config_store_get_blob(
        AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
        encoded,
        &encoded_size);
    if (error == ESP_ERR_NOT_FOUND) {
        *found = false;
        airdap_wifi_credentials_clear(credentials);
        clear_bytes(encoded, sizeof(encoded));
        return ESP_OK;
    }
    if (error != ESP_OK) {
        clear_bytes(encoded, sizeof(encoded));
        return error;
    }
    if (!airdap_wifi_credentials_decode(
            encoded,
            encoded_size,
            credentials)) {
        clear_bytes(encoded, sizeof(encoded));
        return ESP_ERR_INVALID_RESPONSE;
    }

    *found = true;
    clear_bytes(encoded, sizeof(encoded));
    return ESP_OK;
}

static airdap_mode_event_t mode_event_for_state(
    airdap_wifi_sm_state_t state)
{
    switch (state) {
    case AIRDAP_WIFI_SM_STOPPED:
        return AIRDAP_MODE_EVENT_WIFI_STOPPED;
    case AIRDAP_WIFI_SM_DISCONNECTED:
        return AIRDAP_MODE_EVENT_WIFI_DISCONNECTED;
    case AIRDAP_WIFI_SM_CONNECTING:
        return AIRDAP_MODE_EVENT_WIFI_CONNECTING;
    case AIRDAP_WIFI_SM_ONLINE:
        return AIRDAP_MODE_EVENT_WIFI_ONLINE;
    default:
        return AIRDAP_MODE_EVENT_WIFI_STOPPED;
    }
}

static void publish_state(airdap_wifi_sm_state_t state)
{
    const airdap_mode_state_result_t result = airdap_mode_state_transition(
        mode_event_for_state(state));
    if (result != AIRDAP_MODE_STATE_OK) {
        ESP_LOGE(TAG, "Failed to publish Wi-Fi state %d: %d", state, result);
    }
}

static esp_err_t configure_station_from_store(void)
{
    airdap_wifi_credentials_t credentials;
    bool found = false;
    esp_err_t error = load_stored_credentials(&credentials, &found);
    if (error != ESP_OK) {
        return error;
    }
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t config = {0};
    memcpy(
        config.sta.ssid,
        credentials.ssid,
        credentials.ssid_length);
    memcpy(
        config.sta.password,
        credentials.password,
        credentials.password_length);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.failure_retry_cnt = 0U;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    error = esp_wifi_set_config(WIFI_IF_STA, &config);

    clear_bytes(&config, sizeof(config));
    airdap_wifi_credentials_clear(&credentials);
    return error;
}

static void handle_state_event(airdap_wifi_sm_event_t event);

static void connect_now(void)
{
    if (connection_in_progress || link_active) {
        return;
    }
    const esp_err_t error = esp_wifi_connect();
    if (error == ESP_OK) {
        connection_in_progress = true;
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi connect start failed: %s", esp_err_to_name(error));
    handle_state_event(AIRDAP_WIFI_SM_EVENT_CONNECT_FAILED);
}

static void configure_and_connect(void)
{
    const esp_err_t error = configure_station_from_store();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi configuration apply failed: %s", esp_err_to_name(error));
        handle_state_event(AIRDAP_WIFI_SM_EVENT_CONNECT_FAILED);
        return;
    }
    connect_now();
}

static void begin_reconfiguration(void)
{
    if (reconfiguration_pending) {
        return;
    }
    if (!link_active && !connection_in_progress) {
        configure_and_connect();
        return;
    }

    reconfiguration_pending = true;
    const esp_err_t error = esp_wifi_disconnect();
    if (error == ESP_OK) {
        return;
    }
    reconfiguration_pending = false;
    if (error != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Wi-Fi disconnect for reconfiguration failed: %s",
            esp_err_to_name(error));
    }
    link_active = false;
    connection_in_progress = false;
    configure_and_connect();
}

static void disconnect_station(void)
{
    reconfiguration_pending = false;
    if (link_active || connection_in_progress) {
        const esp_err_t error = esp_wifi_disconnect();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "Wi-Fi disconnect failed: %s", esp_err_to_name(error));
        }
    }
    link_active = false;
    connection_in_progress = false;
}

static void cancel_retry(void)
{
    if (retry_timer != NULL && esp_timer_is_active(retry_timer)) {
        const esp_err_t error = esp_timer_stop(retry_timer);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Failed to cancel Wi-Fi retry: %s", esp_err_to_name(error));
        }
    }
}

static void schedule_retry(uint32_t delay_ms)
{
    if (retry_timer == NULL || delay_ms == 0U) {
        return;
    }
    cancel_retry();
    const esp_err_t error = esp_timer_start_once(
        retry_timer,
        (uint64_t) delay_ms * 1000U);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to schedule Wi-Fi retry: %s", esp_err_to_name(error));
    } else {
        ESP_LOGI(TAG, "Wi-Fi retry scheduled in %" PRIu32 " ms", delay_ms);
    }
}

static void apply_effects(const airdap_wifi_sm_effects_t *effects)
{
    if (effects->cancel_retry) {
        cancel_retry();
    }
    if (effects->publish_state) {
        publish_state(effects->published_state);
    }
    if (effects->reconfigure) {
        begin_reconfiguration();
    } else if (effects->disconnect) {
        disconnect_station();
    } else if (effects->connect) {
        connect_now();
    }
    if (effects->retry_after_ms != 0U) {
        schedule_retry(effects->retry_after_ms);
    }
}

static void handle_state_event(airdap_wifi_sm_event_t event)
{
    airdap_wifi_sm_effects_t effects;
    if (!airdap_wifi_state_machine_step(&state_machine, event, &effects)) {
        ESP_LOGE(TAG, "Invalid Wi-Fi state event: %d", event);
        return;
    }
    apply_effects(&effects);
}

static void handle_wifi_event(int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        handle_state_event(AIRDAP_WIFI_SM_EVENT_STA_STARTED);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        connection_in_progress = false;
        link_active = true;
        handle_state_event(AIRDAP_WIFI_SM_EVENT_LINK_CONNECTED);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        connection_in_progress = false;
        link_active = false;
        if (reconfiguration_pending) {
            reconfiguration_pending = false;
            configure_and_connect();
            break;
        }
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        const uint8_t reason = disconnected == NULL
            ? WIFI_REASON_UNSPECIFIED
            : disconnected->reason;
        if (airdap_wifi_disconnect_is_authentication_failure(reason)) {
            ESP_LOGW(TAG, "Wi-Fi authentication failed (reason=%u)", reason);
            handle_state_event(AIRDAP_WIFI_SM_EVENT_AUTHENTICATION_FAILED);
        } else {
            ESP_LOGW(TAG, "Wi-Fi temporarily disconnected (reason=%u)", reason);
            handle_state_event(AIRDAP_WIFI_SM_EVENT_TRANSIENT_DISCONNECT);
        }
        break;
    }
    default:
        break;
    }
}

static void handle_configuration_changed(void)
{
    airdap_wifi_credentials_t credentials;
    bool found = false;
    const esp_err_t error = load_stored_credentials(&credentials, &found);
    airdap_wifi_credentials_clear(&credentials);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Stored Wi-Fi configuration is invalid: %s",
            esp_err_to_name(error));
        handle_state_event(AIRDAP_WIFI_SM_EVENT_CONFIGURATION_CLEARED);
        return;
    }
    handle_state_event(found
        ? AIRDAP_WIFI_SM_EVENT_CONFIGURATION_UPDATED
        : AIRDAP_WIFI_SM_EVENT_CONFIGURATION_CLEARED);
}

static void event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void) argument;
    if (event_base == WIFI_EVENT) {
        handle_wifi_event(event_id, event_data);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        handle_state_event(AIRDAP_WIFI_SM_EVENT_GOT_IP);
    } else if (event_base == AIRDAP_WIFI_INTERNAL_EVENT) {
        if (event_id == AIRDAP_WIFI_INTERNAL_RETRY) {
            handle_state_event(AIRDAP_WIFI_SM_EVENT_RETRY_EXPIRED);
        } else if (event_id == AIRDAP_WIFI_INTERNAL_CONFIGURATION_CHANGED) {
            handle_configuration_changed();
        }
    }
}

static void retry_timer_callback(void *argument)
{
    (void) argument;
    const esp_err_t error = esp_event_post(
        AIRDAP_WIFI_INTERNAL_EVENT,
        AIRDAP_WIFI_INTERNAL_RETRY,
        NULL,
        0U,
        0U);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post Wi-Fi retry: %s", esp_err_to_name(error));
        (void) esp_timer_start_once(
            retry_timer,
            (uint64_t) AIRDAP_WIFI_RETRY_INITIAL_DELAY_MS * 1000U);
    }
}

static void cleanup_failed_start(void)
{
    if (internal_event_instance != NULL) {
        (void) esp_event_handler_instance_unregister(
            AIRDAP_WIFI_INTERNAL_EVENT,
            ESP_EVENT_ANY_ID,
            internal_event_instance);
        internal_event_instance = NULL;
    }
    if (ip_event_instance != NULL) {
        (void) esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            ip_event_instance);
        ip_event_instance = NULL;
    }
    if (wifi_event_instance != NULL) {
        (void) esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_instance);
        wifi_event_instance = NULL;
    }
    if (retry_timer != NULL) {
        (void) esp_timer_delete(retry_timer);
        retry_timer = NULL;
    }
    if (wifi_initialized) {
        (void) esp_wifi_deinit();
        wifi_initialized = false;
    }
    if (station_netif != NULL) {
        esp_netif_destroy_default_wifi(station_netif);
        station_netif = NULL;
    }
    if (event_loop_created) {
        (void) esp_event_loop_delete_default();
        event_loop_created = false;
    }
    started = false;
    link_active = false;
    connection_in_progress = false;
    reconfiguration_pending = false;
}

esp_err_t airdap_wifi_manager_start(void)
{
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }

    airdap_wifi_credentials_t credentials;
    bool has_configuration = false;
    esp_err_t error = load_stored_credentials(
        &credentials,
        &has_configuration);
    airdap_wifi_credentials_clear(&credentials);
    if (error != ESP_OK) {
        return error;
    }
    airdap_wifi_state_machine_init(&state_machine, has_configuration);

    /* config_store owns global NVS initialization and runs before this call.
     * Wi-Fi may use that initialized NVS, but must never initialize or erase it. */
    error = esp_netif_init();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK) {
        return error;
    }
    event_loop_created = true;

    station_netif = esp_netif_create_default_wifi_sta();
    if (station_netif == NULL) {
        cleanup_failed_start();
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init_config);
    if (error != ESP_OK) {
        cleanup_failed_start();
        return error;
    }
    wifi_initialized = true;
    if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        cleanup_failed_start();
        return error;
    }
    if (has_configuration &&
        (error = configure_station_from_store()) != ESP_OK) {
        cleanup_failed_start();
        return error;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_callback,
        .name = "airdap_wifi_retry",
    };
    if ((error = esp_timer_create(&timer_args, &retry_timer)) != ESP_OK ||
        (error = esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            event_handler,
            NULL,
            &wifi_event_instance)) != ESP_OK ||
        (error = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            event_handler,
            NULL,
            &ip_event_instance)) != ESP_OK ||
        (error = esp_event_handler_instance_register(
            AIRDAP_WIFI_INTERNAL_EVENT,
            ESP_EVENT_ANY_ID,
            event_handler,
            NULL,
            &internal_event_instance)) != ESP_OK) {
        cleanup_failed_start();
        return error;
    }

    started = true;
    error = esp_wifi_start();
    if (error != ESP_OK) {
        cleanup_failed_start();
        return error;
    }
    return ESP_OK;
}

static esp_err_t notify_configuration_changed(void)
{
    if (!started) {
        return ESP_OK;
    }
    return esp_event_post(
        AIRDAP_WIFI_INTERNAL_EVENT,
        AIRDAP_WIFI_INTERNAL_CONFIGURATION_CHANGED,
        NULL,
        0U,
        portMAX_DELAY);
}

esp_err_t airdap_wifi_manager_set_credentials(
    const airdap_wifi_credentials_t *credentials)
{
    uint8_t encoded[AIRDAP_WIFI_CREDENTIALS_ENCODED_MAX_SIZE];
    size_t encoded_size = sizeof(encoded);
    if (!airdap_wifi_credentials_encode(
            credentials,
            encoded,
            &encoded_size)) {
        clear_bytes(encoded, sizeof(encoded));
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t error = airdap_config_store_set_blob(
        AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS,
        encoded,
        encoded_size);
    clear_bytes(encoded, sizeof(encoded));
    return error == ESP_OK ? notify_configuration_changed() : error;
}

esp_err_t airdap_wifi_manager_clear_credentials(void)
{
    const esp_err_t error = airdap_config_store_clear(
        AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS);
    return error == ESP_OK ? notify_configuration_changed() : error;
}
