#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_config_store.h"
#include "airdap_mode_state.h"
#include "airdap_wifi_manager.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "wifi_manager_hil";

static void clear_bytes(void *data, size_t size)
{
    volatile uint8_t *byte = (volatile uint8_t *) data;
    for (size_t index = 0U; index < size; ++index) {
        byte[index] = 0U;
    }
}

static bool read_line(const char *prompt, char *buffer, size_t buffer_size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buffer, (int) buffer_size, stdin) == NULL) {
        return false;
    }

    const size_t line_length = strcspn(buffer, "\r\n");
    if (buffer[line_length] == '\0' && line_length == buffer_size - 1U) {
        int character;
        do {
            character = fgetc(stdin);
        } while (character != '\n' && character != EOF);
        buffer[0] = '\0';
        return false;
    }
    buffer[line_length] = '\0';
    return true;
}

static const char *wifi_state_name(airdap_wifi_state_t state)
{
    switch (state) {
    case AIRDAP_WIFI_STOPPED:
        return "stopped";
    case AIRDAP_WIFI_DISCONNECTED:
        return "disconnected";
    case AIRDAP_WIFI_CONNECTING:
        return "connecting";
    case AIRDAP_WIFI_ONLINE:
        return "online";
    default:
        return "invalid";
    }
}

static void print_status(void)
{
    airdap_mode_snapshot_t snapshot;
    const airdap_mode_state_result_t result = airdap_mode_state_get(&snapshot);
    if (result != AIRDAP_MODE_STATE_OK) {
        ESP_LOGE(TAG, "mode state read failed: %d", result);
        return;
    }
    ESP_LOGI(TAG, "STATUS wifi=%s", wifi_state_name(snapshot.wifi));
}

static void set_credentials(void)
{
    char ssid[AIRDAP_WIFI_SSID_MAX_LENGTH + 2U];
    char password[AIRDAP_WIFI_PASSWORD_MAX_LENGTH + 2U];
    if (!read_line("SSID: ", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        ESP_LOGE(TAG, "invalid or overlong SSID");
        clear_bytes(ssid, sizeof(ssid));
        return;
    }
    if (!read_line(
            "Password (input is not logged): ",
            password,
            sizeof(password))) {
        ESP_LOGE(TAG, "overlong password");
        clear_bytes(ssid, sizeof(ssid));
        clear_bytes(password, sizeof(password));
        return;
    }

    airdap_wifi_credentials_t credentials = {0};
    credentials.ssid_length = (uint8_t) strlen(ssid);
    credentials.password_length = (uint8_t) strlen(password);
    memcpy(credentials.ssid, ssid, credentials.ssid_length);
    memcpy(credentials.password, password, credentials.password_length);
    const esp_err_t error = airdap_wifi_manager_set_credentials(&credentials);

    clear_bytes(&credentials, sizeof(credentials));
    clear_bytes(ssid, sizeof(ssid));
    clear_bytes(password, sizeof(password));
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "SET_OK: credentials committed; retry reset requested");
    } else {
        ESP_LOGE(TAG, "SET_FAILED: %s", esp_err_to_name(error));
    }
}

void app_main(void)
{
    airdap_mode_state_init();
    ESP_ERROR_CHECK(airdap_config_store_init());
    ESP_ERROR_CHECK(airdap_wifi_manager_start());
    ESP_LOGI(TAG, "commands: set, clear, status");

    char command[16];
    for (;;) {
        if (!read_line("wifi-hil> ", command, sizeof(command))) {
            ESP_LOGE(TAG, "invalid command input");
            continue;
        }
        if (strcmp(command, "set") == 0) {
            set_credentials();
        } else if (strcmp(command, "clear") == 0) {
            const esp_err_t error = airdap_wifi_manager_clear_credentials();
            if (error == ESP_OK) {
                ESP_LOGI(TAG, "CLEAR_OK");
            } else {
                ESP_LOGE(TAG, "CLEAR_FAILED: %s", esp_err_to_name(error));
            }
        } else if (strcmp(command, "status") == 0) {
            print_status();
        } else {
            ESP_LOGW(TAG, "unknown command");
        }
    }
}
