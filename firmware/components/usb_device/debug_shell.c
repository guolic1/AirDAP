#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell.h"
#include "airdap_debug_shell_input.h"
#include "airdap_debug_shell_tx_state.h"
#include "airdap_voltage_monitor.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

enum {
    DEBUG_VENDOR_INSTANCE = 1,
    SHELL_TASK_STACK_SIZE = 4096,
    SHELL_TASK_PRIORITY = 4,
    SHELL_READ_CHUNK = 64,
    SHELL_POLL_INTERVAL_MS = 10,
    SHELL_WRITE_TIMEOUT_MS = 100,
    RESTART_FLUSH_TIMEOUT_MS = 1000,
    SHELL_FORMAT_BUFFER_SIZE = 384,
    LOG_MIRROR_BUFFER_SIZE = 512,
};

typedef int (*shell_command_handler_t)(const char *arguments);

typedef struct {
    const char *name;
    const char *help;
    shell_command_handler_t handler;
} shell_command_t;

static int help_command(const char *arguments);
static int status_command(const char *arguments);
static int restart_command(const char *arguments);

static const shell_command_t commands[] = {
    {
        .name = "help",
        .help = "List available commands",
        .handler = help_command,
    },
    {
        .name = "status",
        .help = "Show voltages, uptime, and free heap",
        .handler = status_command,
    },
    {
        .name = "restart",
        .help = "Restart the AirDAP firmware",
        .handler = restart_command,
    },
};

static SemaphoreHandle_t output_mutex;
static atomic_bool session_active = ATOMIC_VAR_INIT(false);
static _Atomic(vprintf_like_t) previous_log_vprintf = ATOMIC_VAR_INIT(vprintf);
static portMUX_TYPE tx_state_lock = portMUX_INITIALIZER_UNLOCKED;
static airdap_debug_shell_tx_state_t tx_state;
static TaskHandle_t tx_idle_waiter;

static airdap_debug_shell_tx_reservation_t tx_reserve(size_t requested_bytes)
{
    portENTER_CRITICAL(&tx_state_lock);
    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&tx_state, requested_bytes);
    portEXIT_CRITICAL(&tx_state_lock);
    return reservation;
}

static airdap_debug_shell_tx_ticket_t tx_commit(
    airdap_debug_shell_tx_reservation_t reservation,
    size_t accepted_bytes)
{
    portENTER_CRITICAL(&tx_state_lock);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(
            &tx_state,
            reservation,
            accepted_bytes);
    portEXIT_CRITICAL(&tx_state_lock);
    return ticket;
}

static size_t debug_write(const char *data, size_t length, TickType_t timeout)
{
    if (data == NULL || length == 0U || output_mutex == NULL ||
        !atomic_load(&session_active) ||
        !tud_vendor_n_mounted(DEBUG_VENDOR_INSTANCE)) {
        return 0U;
    }
    if (xSemaphoreTake(output_mutex, timeout) != pdTRUE) {
        return 0U;
    }

    const TickType_t started = xTaskGetTickCount();
    size_t offset = 0U;
    while (offset < length) {
        const airdap_debug_shell_tx_reservation_t reservation =
            tx_reserve(length - offset);
        if (!reservation.valid) {
            break;
        }
        const uint32_t written = tud_vendor_n_write(
            DEBUG_VENDOR_INSTANCE,
            data + offset,
            length - offset);
        const airdap_debug_shell_tx_ticket_t ticket =
            tx_commit(reservation, written);
        if (!ticket.valid) {
            break;
        }
        if (written > 0U) {
            offset += written;
            (void) tud_vendor_n_write_flush(DEBUG_VENDOR_INSTANCE);
            continue;
        }
        if (timeout == 0U || xTaskGetTickCount() - started >= timeout) {
            break;
        }
        vTaskDelay(1);
    }
    xSemaphoreGive(output_mutex);
    return offset;
}

static bool wait_for_tx_ticket(
    airdap_debug_shell_tx_ticket_t ticket,
    TickType_t timeout)
{
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const TickType_t started = xTaskGetTickCount();
    (void) ulTaskNotifyTake(pdTRUE, 0);

    for (;;) {
        portENTER_CRITICAL(&tx_state_lock);
        const airdap_debug_shell_tx_status_t status =
            airdap_debug_shell_tx_state_status(&tx_state, ticket);
        if (status != AIRDAP_DEBUG_SHELL_TX_PENDING) {
            if (tx_idle_waiter == current_task) {
                tx_idle_waiter = NULL;
            }
            portEXIT_CRITICAL(&tx_state_lock);
            return status == AIRDAP_DEBUG_SHELL_TX_COMPLETE;
        }
        tx_idle_waiter = current_task;
        portEXIT_CRITICAL(&tx_state_lock);

        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout ||
            ulTaskNotifyTake(pdTRUE, timeout - elapsed) == 0U) {
            portENTER_CRITICAL(&tx_state_lock);
            const airdap_debug_shell_tx_status_t final_status =
                airdap_debug_shell_tx_state_status(&tx_state, ticket);
            if (tx_idle_waiter == current_task) {
                tx_idle_waiter = NULL;
            }
            portEXIT_CRITICAL(&tx_state_lock);
            return final_status == AIRDAP_DEBUG_SHELL_TX_COMPLETE;
        }
    }
}

static bool write_and_wait(const char *data, size_t length, TickType_t timeout)
{
    if (data == NULL || length == 0U || output_mutex == NULL ||
        !atomic_load(&session_active) ||
        !tud_vendor_n_mounted(DEBUG_VENDOR_INSTANCE) ||
        xSemaphoreTake(output_mutex, timeout) != pdTRUE) {
        return false;
    }

    const TickType_t started = xTaskGetTickCount();
    airdap_debug_shell_tx_ticket_t ticket = {0};
    size_t offset = 0U;
    bool completed = true;
    while (offset < length) {
        if (!atomic_load(&session_active) ||
            !tud_vendor_n_mounted(DEBUG_VENDOR_INSTANCE)) {
            completed = false;
            break;
        }
        const airdap_debug_shell_tx_reservation_t reservation =
            tx_reserve(length - offset);
        if (!reservation.valid) {
            completed = false;
            break;
        }
        const uint32_t written = tud_vendor_n_write(
            DEBUG_VENDOR_INSTANCE,
            data + offset,
            length - offset);
        const airdap_debug_shell_tx_ticket_t committed =
            tx_commit(reservation, written);
        if (!committed.valid) {
            completed = false;
            break;
        }
        if (written > 0U) {
            ticket = committed;
            offset += written;
            (void) tud_vendor_n_write_flush(DEBUG_VENDOR_INSTANCE);
            continue;
        }
        if (xTaskGetTickCount() - started >= timeout) {
            completed = false;
            break;
        }
        vTaskDelay(1);
    }
    if (completed) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        completed = elapsed < timeout &&
            wait_for_tx_ticket(ticket, timeout - elapsed);
    }
    if (completed) {
        completed = atomic_load(&session_active) &&
            tud_vendor_n_mounted(DEBUG_VENDOR_INSTANCE);
    }
    xSemaphoreGive(output_mutex);
    return completed;
}

static void shell_write(const char *data, size_t length, void *context)
{
    (void) context;
    (void) debug_write(data, length, pdMS_TO_TICKS(SHELL_WRITE_TIMEOUT_MS));
}

static void shell_printf(const char *format, ...)
{
    char output[SHELL_FORMAT_BUFFER_SIZE];
    va_list arguments;
    va_start(arguments, format);
    const int formatted = vsnprintf(output, sizeof(output), format, arguments);
    va_end(arguments);

    if (formatted <= 0) {
        return;
    }
    const size_t length = (size_t) formatted < sizeof(output)
        ? (size_t) formatted
        : sizeof(output) - 1U;
    shell_write(output, length, NULL);
}

static int mirror_log_vprintf(const char *format, va_list arguments)
{
    va_list primary_arguments;
    va_copy(primary_arguments, arguments);
    const vprintf_like_t primary = atomic_load(&previous_log_vprintf);
    const int result = primary != NULL ? primary(format, primary_arguments) : 0;
    va_end(primary_arguments);

    if (!atomic_load(&session_active)) {
        return result;
    }

    char output[LOG_MIRROR_BUFFER_SIZE];
    va_list mirror_arguments;
    va_copy(mirror_arguments, arguments);
    const int formatted = vsnprintf(
        output,
        sizeof(output),
        format,
        mirror_arguments);
    va_end(mirror_arguments);
    if (formatted > 0) {
        const size_t length = (size_t) formatted < sizeof(output)
            ? (size_t) formatted
            : sizeof(output) - 1U;
        (void) debug_write(output, length, 0U);
    }
    return result;
}

static void shell_execute(const char *line, void *context)
{
    (void) context;
    while (*line == ' ') {
        ++line;
    }

    const char *command_end = line;
    while (*command_end != '\0' && *command_end != ' ') {
        ++command_end;
    }
    const size_t command_length = (size_t) (command_end - line);
    if (command_length == 0U) {
        return;
    }

    const char *arguments = command_end;
    while (*arguments == ' ') {
        ++arguments;
    }

    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const shell_command_t *command = &commands[index];
        if (strlen(command->name) == command_length &&
            strncmp(line, command->name, command_length) == 0) {
            (void) command->handler(arguments);
            return;
        }
    }

    shell_printf("Unrecognized command: %.*s\n", (int) command_length, line);
}

static int help_command(const char *arguments)
{
    if (*arguments != '\0') {
        shell_printf("usage: help\n");
        return 1;
    }

    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        shell_printf("%-8s %s\n", commands[index].name, commands[index].help);
    }
    return 0;
}

static int status_command(const char *arguments)
{
    if (*arguments != '\0') {
        shell_printf("usage: status\n");
        return 1;
    }

    airdap_voltage_reading_t voltage;
    const esp_err_t error = airdap_voltage_monitor_read(&voltage);
    if (error != ESP_OK) {
        shell_printf("status: voltage read failed: %s\n", esp_err_to_name(error));
        return 1;
    }

    shell_printf(
        "target_mv=%" PRIu32 " usb_vbus_mv=%" PRIu32
        " uptime_ms=%" PRId64 " free_heap=%" PRIu32 "\n",
        voltage.target_mv,
        voltage.usb_vbus_mv,
        esp_timer_get_time() / 1000,
        esp_get_free_heap_size());
    return 0;
}

static int restart_command(const char *arguments)
{
    static const char acknowledgement[] = "Restarting AirDAP...\n";

    if (*arguments != '\0') {
        shell_printf("usage: restart\n");
        return 1;
    }

    if (!write_and_wait(
        acknowledgement,
        sizeof(acknowledgement) - 1U,
        pdMS_TO_TICKS(RESTART_FLUSH_TIMEOUT_MS))) {
        shell_printf("restart: acknowledgement transfer failed\n");
        return 1;
    }
    if (!atomic_load(&session_active) ||
        !tud_vendor_n_mounted(DEBUG_VENDOR_INSTANCE)) {
        return 1;
    }
    esp_restart();
    return 0;
}

static void start_session(airdap_debug_shell_input_t *input)
{
    static const char banner[] =
        "\nAirDAP debug shell\n"
        "Type 'help' to list commands. Ctrl-] exits airdap-shell.\n"
        "airdap> ";

    if (xSemaphoreTake(output_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&tx_state_lock);
    airdap_debug_shell_tx_state_connected(&tx_state);
    atomic_store(&session_active, true);
    portEXIT_CRITICAL(&tx_state_lock);
    xSemaphoreGive(output_mutex);
    airdap_debug_shell_input_init(input);
    shell_write(banner, sizeof(banner) - 1U, NULL);
}

static void end_session(void)
{
    atomic_store(&session_active, false);
}

static void shell_task(void *argument)
{
    (void) argument;
    airdap_debug_shell_input_t input;
    const airdap_debug_shell_input_callbacks_t callbacks = {
        .write = shell_write,
        .execute = shell_execute,
        .context = NULL,
    };
    uint8_t data[SHELL_READ_CHUNK];

    airdap_debug_shell_input_init(&input);
    for (;;) {
        if (!tud_vendor_n_mounted(DEBUG_VENDOR_INSTANCE)) {
            airdap_debug_shell_disconnected();
            airdap_debug_shell_input_init(&input);
            vTaskDelay(pdMS_TO_TICKS(SHELL_POLL_INTERVAL_MS));
            continue;
        }

        const uint32_t received = tud_vendor_n_read(
            DEBUG_VENDOR_INSTANCE,
            data,
            sizeof(data));
        if (received == 0U) {
            vTaskDelay(pdMS_TO_TICKS(SHELL_POLL_INTERVAL_MS));
            continue;
        }

        for (uint32_t index = 0U; index < received; ++index) {
            if (data[index] == 0x00U) {
                start_session(&input);
            } else if (data[index] == 0x04U) {
                end_session();
                airdap_debug_shell_input_init(&input);
            } else if (atomic_load(&session_active)) {
                airdap_debug_shell_input_consume(
                    &input,
                    &data[index],
                    1U,
                    &callbacks);
            }
        }
    }
}

esp_err_t airdap_debug_shell_start(void)
{
    airdap_debug_shell_tx_state_init(&tx_state);
    output_mutex = xSemaphoreCreateMutex();
    if (output_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(
        shell_task,
        "debug_shell",
        SHELL_TASK_STACK_SIZE,
        NULL,
        SHELL_TASK_PRIORITY,
        NULL,
        1) != pdPASS) {
        vSemaphoreDelete(output_mutex);
        output_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    const vprintf_like_t previous = esp_log_set_vprintf(mirror_log_vprintf);
    atomic_store(&previous_log_vprintf, previous);
    return ESP_OK;
}

void airdap_debug_shell_disconnected(void)
{
    atomic_store(&session_active, false);

    TaskHandle_t waiter;
    portENTER_CRITICAL(&tx_state_lock);
    airdap_debug_shell_tx_state_disconnected(&tx_state);
    waiter = tx_idle_waiter;
    tx_idle_waiter = NULL;
    portEXIT_CRITICAL(&tx_state_lock);
    if (waiter != NULL) {
        xTaskNotifyGive(waiter);
    }
}

void airdap_debug_shell_tx_complete(uint32_t sent_bytes)
{
    TaskHandle_t waiter;

    portENTER_CRITICAL(&tx_state_lock);
    airdap_debug_shell_tx_state_completed(&tx_state, sent_bytes);
    waiter = tx_idle_waiter;
    portEXIT_CRITICAL(&tx_state_lock);
    if (waiter != NULL) {
        xTaskNotifyGive(waiter);
    }
}
