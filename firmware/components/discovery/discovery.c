#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_device_identity.h"
#include "airdap_discovery.h"
#include "airdap_mode_state.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"

enum {
    AIRDAP_DAP_TCP_PORT = 3260,
    AIRDAP_UART_TCP_PORT = 3261,
    AIRDAP_DISCOVERY_TXT_ITEM_COUNT = 7,
    AIRDAP_DISCOVERY_HOSTNAME_SIZE = 24,
    AIRDAP_DISCOVERY_CAPABILITIES_SIZE = 48,
};

typedef enum {
    AIRDAP_DISCOVERY_INTERNAL_RECONCILE = 0,
} airdap_discovery_internal_event_t;

typedef struct {
    uint32_t bit;
    const char *name;
} capability_name_t;

ESP_EVENT_DEFINE_BASE(AIRDAP_DISCOVERY_INTERNAL_EVENT);

static const char *TAG = "airdap_discovery";
static const char *SERVICE_TYPE = "_airdap";
static const char *SERVICE_PROTOCOL = "_tcp";
static const capability_name_t CAPABILITY_NAMES[] = {
    {AIRDAP_CAPABILITY_SWD, "swd"},
    {AIRDAP_CAPABILITY_TARGET_UART, "uart"},
    {AIRDAP_CAPABILITY_TARGET_POWER, "power"},
    {AIRDAP_CAPABILITY_TARGET_RESET, "reset"},
    {AIRDAP_CAPABILITY_USB_OTA, "ota"},
};

static esp_event_handler_instance_t ip_event_instance;
static esp_event_handler_instance_t internal_event_instance;
static bool mdns_initialized;
static bool started;
static bool service_published;

static esp_err_t format_hostname(
    const char *device_id,
    char hostname[AIRDAP_DISCOVERY_HOSTNAME_SIZE])
{
    static const char device_id_prefix[] = "ADP-";
    if (device_id == NULL ||
        strncmp(device_id, device_id_prefix, sizeof(device_id_prefix) - 1U) != 0 ||
        strlen(device_id) != AIRDAP_DEVICE_SERIAL_LENGTH) {
        return ESP_FAIL;
    }

    static const char hostname_prefix[] = "airdap-";
    memcpy(hostname, hostname_prefix, sizeof(hostname_prefix) - 1U);
    const char *suffix = device_id + sizeof(device_id_prefix) - 1U;
    for (size_t index = 0U; suffix[index] != '\0'; ++index) {
        if (!isxdigit((unsigned char) suffix[index])) {
            return ESP_FAIL;
        }
        hostname[sizeof(hostname_prefix) - 1U + index] =
            (char) tolower((unsigned char) suffix[index]);
    }
    hostname[sizeof(hostname_prefix) - 1U +
        AIRDAP_DEVICE_SERIAL_LENGTH - (sizeof(device_id_prefix) - 1U)] = '\0';
    return ESP_OK;
}

static esp_err_t format_capabilities(
    uint32_t capabilities,
    char value[AIRDAP_DISCOVERY_CAPABILITIES_SIZE])
{
    size_t used = 0U;
    for (size_t index = 0U;
         index < sizeof(CAPABILITY_NAMES) / sizeof(CAPABILITY_NAMES[0]);
         ++index) {
        if ((capabilities & CAPABILITY_NAMES[index].bit) == 0U) {
            continue;
        }
        const int written = snprintf(
            value + used,
            AIRDAP_DISCOVERY_CAPABILITIES_SIZE - used,
            "%s%s",
            used == 0U ? "" : ",",
            CAPABILITY_NAMES[index].name);
        if (written < 0 ||
            (size_t) written >= AIRDAP_DISCOVERY_CAPABILITIES_SIZE - used) {
            return ESP_FAIL;
        }
        used += (size_t) written;
    }
    return ESP_OK;
}

static esp_err_t withdraw_service(void)
{
    if (!service_published) {
        return ESP_OK;
    }

    const esp_err_t error = mdns_service_remove(
        SERVICE_TYPE,
        SERVICE_PROTOCOL);
    if (error == ESP_OK) {
        service_published = false;
        ESP_LOGI(TAG, "mDNS service withdrawn");
    }
    return error;
}

static esp_err_t publish_service(void)
{
    esp_err_t error = withdraw_service();
    if (error != ESP_OK) {
        return error;
    }

    const airdap_device_identity_t *identity = airdap_device_identity_get();
    if (identity == NULL || identity->firmware_version == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char protocol_version[4];
    char dap_port[6];
    char uart_port[6];
    char capabilities[AIRDAP_DISCOVERY_CAPABILITIES_SIZE] = {0};
    const int protocol_length = snprintf(
        protocol_version,
        sizeof(protocol_version),
        "%u",
        identity->protocol_version);
    const int dap_port_length = snprintf(
        dap_port,
        sizeof(dap_port),
        "%u",
        AIRDAP_DAP_TCP_PORT);
    const int uart_port_length = snprintf(
        uart_port,
        sizeof(uart_port),
        "%u",
        AIRDAP_UART_TCP_PORT);
    if (protocol_length < 0 ||
        (size_t) protocol_length >= sizeof(protocol_version) ||
        dap_port_length < 0 ||
        (size_t) dap_port_length >= sizeof(dap_port) ||
        uart_port_length < 0 ||
        (size_t) uart_port_length >= sizeof(uart_port) ||
        format_capabilities(identity->capabilities, capabilities) != ESP_OK) {
        return ESP_FAIL;
    }

    mdns_txt_item_t txt[AIRDAP_DISCOVERY_TXT_ITEM_COUNT] = {
        {"id", identity->device_id},
        {"proto", protocol_version},
        {"fw", identity->firmware_version},
        {"cap", capabilities},
        {"state", "idle"},
        {"dap_port", dap_port},
        {"uart_port", uart_port},
    };
    error = mdns_service_add(
        identity->device_id,
        SERVICE_TYPE,
        SERVICE_PROTOCOL,
        AIRDAP_DAP_TCP_PORT,
        txt,
        AIRDAP_DISCOVERY_TXT_ITEM_COUNT);
    if (error == ESP_OK) {
        service_published = true;
        ESP_LOGI(TAG, "mDNS service published");
    }
    return error;
}

static void reconcile_service(bool refresh_online)
{
    airdap_mode_snapshot_t mode;
    const airdap_mode_state_result_t result = airdap_mode_state_get(&mode);
    esp_err_t error;
    if (result != AIRDAP_MODE_STATE_OK) {
        ESP_LOGE(TAG, "Cannot reconcile mDNS with mode state: %d", result);
        return;
    }
    if (mode.wifi == AIRDAP_WIFI_ONLINE) {
        if ((refresh_online || !service_published) &&
            (error = publish_service()) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to publish mDNS service: %d", error);
        }
    } else if ((error = withdraw_service()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to withdraw mDNS service: %d", error);
    }
}

static void event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void) argument;
    (void) event_data;
    if (!started) {
        return;
    }

    esp_err_t error = ESP_OK;
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            reconcile_service(true);
            return;
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            error = withdraw_service();
        }
    } else if (event_base == AIRDAP_DISCOVERY_INTERNAL_EVENT &&
               event_id == AIRDAP_DISCOVERY_INTERNAL_RECONCILE) {
        reconcile_service(false);
        return;
    }

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "mDNS lifecycle update failed: %d", error);
    }
}

static void cleanup_failed_start(void)
{
    if (internal_event_instance != NULL) {
        (void) esp_event_handler_instance_unregister(
            AIRDAP_DISCOVERY_INTERNAL_EVENT,
            ESP_EVENT_ANY_ID,
            internal_event_instance);
        internal_event_instance = NULL;
    }
    if (ip_event_instance != NULL) {
        (void) esp_event_handler_instance_unregister(
            IP_EVENT,
            ESP_EVENT_ANY_ID,
            ip_event_instance);
        ip_event_instance = NULL;
    }
    if (mdns_initialized) {
        mdns_free();
        mdns_initialized = false;
    }
    started = false;
    service_published = false;
}

esp_err_t airdap_discovery_start(void)
{
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }

    const airdap_device_identity_t *identity = airdap_device_identity_get();
    char hostname[AIRDAP_DISCOVERY_HOSTNAME_SIZE];
    if (identity == NULL || identity->firmware_version == NULL ||
        format_hostname(identity->device_id, hostname) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = mdns_init();
    if (error != ESP_OK) {
        return error;
    }
    mdns_initialized = true;
    if ((error = mdns_hostname_set(hostname)) != ESP_OK ||
        (error = mdns_instance_name_set(identity->device_id)) != ESP_OK ||
        (error = esp_event_handler_instance_register(
            IP_EVENT,
            ESP_EVENT_ANY_ID,
            event_handler,
            NULL,
            &ip_event_instance)) != ESP_OK ||
        (error = esp_event_handler_instance_register(
            AIRDAP_DISCOVERY_INTERNAL_EVENT,
            ESP_EVENT_ANY_ID,
            event_handler,
            NULL,
            &internal_event_instance)) != ESP_OK) {
        cleanup_failed_start();
        return error;
    }

    started = true;
    error = esp_event_post(
        AIRDAP_DISCOVERY_INTERNAL_EVENT,
        AIRDAP_DISCOVERY_INTERNAL_RECONCILE,
        NULL,
        0U,
        portMAX_DELAY);
    if (error != ESP_OK) {
        cleanup_failed_start();
        return error;
    }
    return ESP_OK;
}
