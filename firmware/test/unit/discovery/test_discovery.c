#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_device_identity.h"
#include "airdap_discovery.h"
#include "airdap_mode_state.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mdns.h"

enum {
    MAX_HANDLERS = 8,
    MAX_TXT_ITEMS = 8,
    MAX_TEXT_SIZE = 64,
};

typedef struct {
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t handler;
    void *argument;
    esp_event_handler_instance_t instance;
    bool active;
} handler_registration_t;

typedef struct {
    char key[MAX_TEXT_SIZE];
    char value[MAX_TEXT_SIZE];
} copied_txt_item_t;

ESP_EVENT_DEFINE_BASE(IP_EVENT);

static const airdap_device_identity_t identity = {
    .usb_serial = "ADP-001122334455",
    .device_id = "ADP-001122334455",
    .firmware_version = "V1.2.3-test",
    .protocol_version = 1,
    .capabilities =
        AIRDAP_CAPABILITY_SWD |
        AIRDAP_CAPABILITY_TARGET_UART |
        AIRDAP_CAPABILITY_TARGET_POWER |
        AIRDAP_CAPABILITY_TARGET_RESET |
        AIRDAP_CAPABILITY_USB_OTA,
};

static const airdap_device_identity_t *identity_result;
static airdap_mode_state_result_t mode_result = AIRDAP_MODE_STATE_OK;
static airdap_wifi_state_t wifi_state = AIRDAP_WIFI_STOPPED;
static esp_err_t mdns_init_result = ESP_OK;
static esp_err_t mdns_hostname_result = ESP_OK;
static esp_err_t mdns_instance_result = ESP_OK;
static esp_err_t mdns_add_result = ESP_OK;
static esp_err_t mdns_remove_result = ESP_OK;
static esp_err_t register_result = ESP_OK;
static esp_err_t post_result = ESP_OK;
static unsigned int mdns_init_calls;
static unsigned int mdns_free_calls;
static unsigned int hostname_calls;
static unsigned int instance_calls;
static unsigned int add_calls;
static unsigned int remove_calls;
static unsigned int register_calls;
static unsigned int unregister_calls;
static unsigned int post_calls;
static char hostname[MAX_TEXT_SIZE];
static char instance_name[MAX_TEXT_SIZE];
static char service_instance[MAX_TEXT_SIZE];
static char service_type[MAX_TEXT_SIZE];
static char service_protocol[MAX_TEXT_SIZE];
static uint16_t service_port;
static copied_txt_item_t copied_txt[MAX_TXT_ITEMS];
static size_t copied_txt_count;
static handler_registration_t handlers[MAX_HANDLERS];

static void copy_text(char destination[MAX_TEXT_SIZE], const char *source)
{
    assert(source != NULL);
    const size_t length = strlen(source);
    assert(length < MAX_TEXT_SIZE);
    memcpy(destination, source, length + 1U);
}

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return identity_result;
}

airdap_mode_state_result_t airdap_mode_state_get(
    airdap_mode_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    if (mode_result == AIRDAP_MODE_STATE_OK) {
        snapshot->wifi = wifi_state;
    }
    return mode_result;
}

esp_err_t mdns_init(void)
{
    ++mdns_init_calls;
    return mdns_init_result;
}

void mdns_free(void)
{
    ++mdns_free_calls;
}

esp_err_t mdns_hostname_set(const char *value)
{
    ++hostname_calls;
    copy_text(hostname, value);
    return mdns_hostname_result;
}

esp_err_t mdns_instance_name_set(const char *value)
{
    ++instance_calls;
    copy_text(instance_name, value);
    return mdns_instance_result;
}

esp_err_t mdns_service_add(
    const char *value,
    const char *type,
    const char *protocol,
    uint16_t port,
    mdns_txt_item_t txt_items[],
    size_t txt_item_count)
{
    ++add_calls;
    copy_text(service_instance, value);
    copy_text(service_type, type);
    copy_text(service_protocol, protocol);
    service_port = port;
    assert(txt_item_count <= MAX_TXT_ITEMS);
    copied_txt_count = txt_item_count;
    for (size_t index = 0U; index < txt_item_count; ++index) {
        copy_text(copied_txt[index].key, txt_items[index].key);
        copy_text(copied_txt[index].value, txt_items[index].value);
    }
    return mdns_add_result;
}

esp_err_t mdns_service_remove(const char *type, const char *protocol)
{
    ++remove_calls;
    copy_text(service_type, type);
    copy_text(service_protocol, protocol);
    return mdns_remove_result;
}

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void *event_handler_arg,
    esp_event_handler_instance_t *instance)
{
    ++register_calls;
    if (register_result != ESP_OK) {
        return register_result;
    }
    assert(register_calls <= MAX_HANDLERS);
    handler_registration_t *registration = &handlers[register_calls - 1U];
    registration->base = event_base;
    registration->id = event_id;
    registration->handler = event_handler;
    registration->argument = event_handler_arg;
    registration->instance = (void *) (uintptr_t) register_calls;
    registration->active = true;
    *instance = registration->instance;
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_instance_t instance)
{
    ++unregister_calls;
    for (size_t index = 0U; index < MAX_HANDLERS; ++index) {
        if (handlers[index].active && handlers[index].base == event_base &&
            handlers[index].id == event_id &&
            handlers[index].instance == instance) {
            handlers[index].active = false;
            return ESP_OK;
        }
    }
    assert(false);
    return ESP_FAIL;
}

static void dispatch_event(
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    for (size_t index = 0U; index < MAX_HANDLERS; ++index) {
        if (handlers[index].active && handlers[index].base == event_base &&
            (handlers[index].id == ESP_EVENT_ANY_ID ||
             handlers[index].id == event_id)) {
            handlers[index].handler(
                handlers[index].argument,
                event_base,
                event_id,
                event_data);
        }
    }
}

esp_err_t esp_event_post(
    esp_event_base_t event_base,
    int32_t event_id,
    const void *event_data,
    size_t event_data_size,
    uint32_t ticks_to_wait)
{
    (void) ticks_to_wait;
    ++post_calls;
    assert(event_data == NULL);
    assert(event_data_size == 0U);
    if (post_result == ESP_OK) {
        dispatch_event(event_base, event_id, NULL);
    }
    return post_result;
}

static void assert_txt(size_t index, const char *key, const char *value)
{
    assert(index < copied_txt_count);
    assert(strcmp(copied_txt[index].key, key) == 0);
    assert(strcmp(copied_txt[index].value, value) == 0);
}

static void test_start_failure_is_clean_and_retryable(void)
{
    identity_result = NULL;
    assert(airdap_discovery_start() == ESP_ERR_INVALID_STATE);
    assert(mdns_init_calls == 0U);

    identity_result = &identity;
    mdns_hostname_result = ESP_FAIL;
    assert(airdap_discovery_start() == ESP_FAIL);
    assert(mdns_init_calls == 1U);
    assert(mdns_free_calls == 1U);
    assert(register_calls == 0U);

    mdns_hostname_result = ESP_OK;
    post_result = ESP_FAIL;
    assert(airdap_discovery_start() == ESP_FAIL);
    assert(mdns_init_calls == 2U);
    assert(mdns_free_calls == 2U);
    assert(register_calls == 2U);
    assert(unregister_calls == 2U);
}

static void test_start_configures_stable_identity_without_advertising_offline(void)
{
    post_result = ESP_OK;
    assert(airdap_discovery_start() == ESP_OK);
    assert(strcmp(hostname, "airdap-001122334455") == 0);
    assert(strcmp(instance_name, identity.device_id) == 0);
    assert(mdns_init_calls == 3U);
    assert(hostname_calls == 3U);
    assert(instance_calls == 2U);
    assert(register_calls == 4U);
    assert(post_calls == 2U);
    assert(add_calls == 0U);
    assert(remove_calls == 0U);
    assert(airdap_discovery_start() == ESP_ERR_INVALID_STATE);
}

static void test_reconcile_and_ip_events_control_exact_service_record(void)
{
    wifi_state = AIRDAP_WIFI_ONLINE;
    esp_event_base_t internal_base = handlers[3].base;
    assert(internal_base != IP_EVENT);
    dispatch_event(internal_base, 0, NULL);

    assert(add_calls == 1U);
    assert(strcmp(service_instance, identity.device_id) == 0);
    assert(strcmp(service_type, "_airdap") == 0);
    assert(strcmp(service_protocol, "_tcp") == 0);
    assert(service_port == 3260U);
    assert(copied_txt_count == 7U);
    assert_txt(0U, "id", identity.device_id);
    assert_txt(1U, "proto", "1");
    assert_txt(2U, "fw", identity.firmware_version);
    assert_txt(3U, "cap", "swd,uart,power,reset,ota");
    assert_txt(4U, "state", "idle");
    assert_txt(5U, "dap_port", "3260");
    assert_txt(6U, "uart_port", "3261");

    wifi_state = AIRDAP_WIFI_CONNECTING;
    dispatch_event(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(remove_calls == 1U);
    assert(add_calls == 1U);

    wifi_state = AIRDAP_WIFI_ONLINE;
    dispatch_event(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(remove_calls == 1U);
    assert(add_calls == 2U);

    dispatch_event(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(remove_calls == 2U);
    assert(add_calls == 3U);

    mdns_remove_result = ESP_FAIL;
    dispatch_event(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(remove_calls == 3U);
    assert(add_calls == 3U);

    mdns_remove_result = ESP_OK;
    dispatch_event(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(remove_calls == 4U);
    assert(add_calls == 3U);

    mdns_add_result = ESP_FAIL;
    dispatch_event(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(add_calls == 4U);
    dispatch_event(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(remove_calls == 4U);

    mdns_add_result = ESP_OK;
    dispatch_event(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(add_calls == 5U);
    dispatch_event(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(remove_calls == 5U);
}

int main(void)
{
    test_start_failure_is_clean_and_retryable();
    test_start_configures_stable_identity_without_advertising_offline();
    test_reconcile_and_ip_events_control_exact_service_record();
    puts("discovery lifecycle tests passed");
    return 0;
}
