#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_config_store.h"
#include "airdap_mode_state.h"
#include "airdap_wifi_credentials.h"
#include "airdap_wifi_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

enum {
    MAX_REGISTRATIONS = 3,
    MAX_POSTED_EVENTS = 8,
};

typedef struct {
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t handler;
    void *argument;
} registration_t;

typedef struct {
    esp_event_base_t base;
    int32_t id;
} posted_event_t;

struct fake_esp_timer {
    void (*callback)(void *argument);
    void *argument;
    bool active;
    uint64_t timeout_us;
};

esp_event_base_t WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t IP_EVENT = "IP_EVENT";

static registration_t registrations[MAX_REGISTRATIONS];
static size_t registration_count;
static posted_event_t posted_events[MAX_POSTED_EVENTS];
static size_t posted_event_count;
static struct fake_esp_timer retry_timer;
static esp_netif_t station_netif;
static uint8_t stored_blob[AIRDAP_WIFI_CREDENTIALS_ENCODED_MAX_SIZE];
static size_t stored_blob_size;
static airdap_mode_event_t last_mode_event;
static size_t mode_transition_count;
static size_t wifi_set_config_count;
static wifi_config_t last_wifi_config;
static size_t wifi_connect_count;
static size_t wifi_disconnect_count;
static size_t timer_start_count;
static size_t timer_stop_count;

static void dispatch(
    esp_event_base_t base,
    int32_t id,
    void *event_data)
{
    for (size_t index = 0U; index < registration_count; ++index) {
        const registration_t *registration = &registrations[index];
        if (registration->base == base &&
            (registration->id == ESP_EVENT_ANY_ID ||
                registration->id == id)) {
            registration->handler(
                registration->argument,
                base,
                id,
                event_data);
        }
    }
}

static void dispatch_next_posted_event(void)
{
    assert(posted_event_count > 0U);
    const posted_event_t event = posted_events[0];
    memmove(
        &posted_events[0],
        &posted_events[1],
        (posted_event_count - 1U) * sizeof(posted_events[0]));
    --posted_event_count;
    dispatch(event.base, event.id, NULL);
}

static void emit_wifi_event(int32_t id, uint8_t reason)
{
    wifi_event_sta_disconnected_t event = {
        .reason = reason,
    };
    dispatch(WIFI_EVENT, id, &event);
}

static void emit_ip_event(int32_t id)
{
    dispatch(IP_EVENT, id, NULL);
}

static airdap_wifi_credentials_t make_credentials(
    const char *ssid,
    const char *password)
{
    airdap_wifi_credentials_t credentials = {0};
    credentials.ssid_length = (uint8_t) strlen(ssid);
    credentials.password_length = (uint8_t) strlen(password);
    memcpy(credentials.ssid, ssid, credentials.ssid_length);
    memcpy(credentials.password, password, credentials.password_length);
    return credentials;
}

esp_err_t airdap_config_store_get_blob(
    airdap_config_slot_t slot,
    void *output,
    size_t *inout_size)
{
    assert(slot == AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS);
    assert(inout_size != NULL);
    if (stored_blob_size == 0U) {
        *inout_size = 0U;
        return ESP_ERR_NOT_FOUND;
    }
    assert(output != NULL);
    assert(*inout_size >= stored_blob_size);
    memcpy(output, stored_blob, stored_blob_size);
    *inout_size = stored_blob_size;
    return ESP_OK;
}

esp_err_t airdap_config_store_set_blob(
    airdap_config_slot_t slot,
    const void *data,
    size_t data_size)
{
    assert(slot == AIRDAP_CONFIG_SLOT_WIFI_CREDENTIALS);
    assert(data != NULL);
    assert(data_size <= sizeof(stored_blob));
    memcpy(stored_blob, data, data_size);
    stored_blob_size = data_size;
    return ESP_OK;
}

esp_err_t airdap_config_store_clear(uint32_t flags)
{
    assert(flags == AIRDAP_CONFIG_CLEAR_WIFI_CREDENTIALS);
    memset(stored_blob, 0, sizeof(stored_blob));
    stored_blob_size = 0U;
    return ESP_OK;
}

airdap_mode_state_result_t airdap_mode_state_transition(
    airdap_mode_event_t event)
{
    last_mode_event = event;
    ++mode_transition_count;
    return AIRDAP_MODE_STATE_OK;
}

esp_err_t esp_event_loop_create_default(void)
{
    return ESP_OK;
}

esp_err_t esp_event_loop_delete_default(void)
{
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void *event_handler_arg,
    esp_event_handler_instance_t *instance)
{
    assert(registration_count < MAX_REGISTRATIONS);
    registration_t *registration = &registrations[registration_count++];
    registration->base = event_base;
    registration->id = event_id;
    registration->handler = event_handler;
    registration->argument = event_handler_arg;
    *instance = registration;
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
    (void) event_data;
    (void) event_data_size;
    (void) ticks_to_wait;
    assert(posted_event_count < MAX_POSTED_EVENTS);
    posted_events[posted_event_count++] = (posted_event_t) {
        .base = event_base,
        .id = event_id,
    };
    return ESP_OK;
}

esp_err_t esp_netif_init(void)
{
    return ESP_OK;
}

esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    return &station_netif;
}

void esp_netif_destroy_default_wifi(void *netif)
{
    assert(netif == &station_netif);
}

esp_err_t esp_timer_create(
    const esp_timer_create_args_t *args,
    esp_timer_handle_t *timer)
{
    assert(args != NULL);
    retry_timer.callback = args->callback;
    retry_timer.argument = args->arg;
    retry_timer.active = false;
    *timer = &retry_timer;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    assert(timer == &retry_timer);
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t timer)
{
    assert(timer == &retry_timer);
    return timer->active;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    assert(timer == &retry_timer);
    timer->active = false;
    ++timer_stop_count;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(
    esp_timer_handle_t timer,
    uint64_t timeout_us)
{
    assert(timer == &retry_timer);
    timer->active = true;
    timer->timeout_us = timeout_us;
    ++timer_start_count;
    return ESP_OK;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    assert(config != NULL);
    return ESP_OK;
}

esp_err_t esp_wifi_deinit(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_set_storage(int storage)
{
    assert(storage == WIFI_STORAGE_RAM);
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(int mode)
{
    assert(mode == WIFI_MODE_STA);
    return ESP_OK;
}

esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{
    assert(interface == WIFI_IF_STA);
    assert(config != NULL);
    last_wifi_config = *config;
    ++wifi_set_config_count;
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_connect(void)
{
    ++wifi_connect_count;
    return ESP_OK;
}

esp_err_t esp_wifi_disconnect(void)
{
    ++wifi_disconnect_count;
    return ESP_OK;
}

int main(void)
{
    assert(airdap_wifi_manager_start() == ESP_OK);
    emit_wifi_event(WIFI_EVENT_STA_START, WIFI_REASON_UNSPECIFIED);
    assert(wifi_connect_count == 0U);

    airdap_wifi_credentials_t credentials = make_credentials(
        "first-ap",
        "first-password");
    assert(airdap_wifi_manager_set_credentials(&credentials) == ESP_OK);
    dispatch_next_posted_event();
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_CONNECTING);
    assert(wifi_set_config_count == 1U);
    assert(wifi_connect_count == 1U);

    credentials = make_credentials("second-ap", "second-password");
    assert(airdap_wifi_manager_set_credentials(&credentials) == ESP_OK);
    dispatch_next_posted_event();
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_CONNECTING);
    assert(wifi_disconnect_count == 1U);

    const size_t transitions_before_old_ip = mode_transition_count;
    emit_wifi_event(WIFI_EVENT_STA_CONNECTED, WIFI_REASON_UNSPECIFIED);
    emit_ip_event(IP_EVENT_STA_GOT_IP);
    assert(mode_transition_count == transitions_before_old_ip);

    emit_wifi_event(WIFI_EVENT_STA_DISCONNECTED, WIFI_REASON_UNSPECIFIED);
    assert(wifi_set_config_count == 2U);
    assert(wifi_connect_count == 2U);
    assert(memcmp(last_wifi_config.sta.ssid, "second-ap", 9U) == 0);
    emit_ip_event(IP_EVENT_STA_GOT_IP);
    assert(mode_transition_count == transitions_before_old_ip);

    emit_wifi_event(WIFI_EVENT_STA_CONNECTED, WIFI_REASON_UNSPECIFIED);
    emit_ip_event(IP_EVENT_STA_GOT_IP);
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_ONLINE);

    emit_ip_event(IP_EVENT_STA_LOST_IP);
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_CONNECTING);
    emit_ip_event(IP_EVENT_STA_GOT_IP);
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_ONLINE);

    emit_wifi_event(WIFI_EVENT_STA_DISCONNECTED, WIFI_REASON_BEACON_TIMEOUT);
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_DISCONNECTED);
    assert(retry_timer.active);
    assert(retry_timer.timeout_us == 1000000U);
    assert(timer_start_count == 1U);

    retry_timer.active = false;
    retry_timer.callback(retry_timer.argument);
    dispatch_next_posted_event();
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_CONNECTING);
    assert(wifi_connect_count == 3U);

    emit_wifi_event(WIFI_EVENT_STA_DISCONNECTED, WIFI_REASON_AUTH_FAIL);
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_DISCONNECTED);
    assert(retry_timer.active);
    assert(retry_timer.timeout_us == 2000000U);

    credentials = make_credentials("third-ap", "third-password");
    assert(airdap_wifi_manager_set_credentials(&credentials) == ESP_OK);
    dispatch_next_posted_event();
    assert(!retry_timer.active);
    assert(timer_stop_count == 1U);
    assert(wifi_set_config_count == 3U);
    assert(wifi_connect_count == 4U);
    assert(memcmp(last_wifi_config.sta.ssid, "third-ap", 8U) == 0);

    emit_wifi_event(WIFI_EVENT_STA_CONNECTED, WIFI_REASON_UNSPECIFIED);
    emit_ip_event(IP_EVENT_STA_GOT_IP);
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_ONLINE);

    assert(airdap_wifi_manager_clear_credentials() == ESP_OK);
    dispatch_next_posted_event();
    assert(last_mode_event == AIRDAP_MODE_EVENT_WIFI_STOPPED);
    assert(wifi_disconnect_count == 2U);

    puts("wifi manager adapter tests passed");
    return 0;
}
