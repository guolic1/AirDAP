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
#include "airdap_debug_shell_swd_probe.h"
#include "airdap_debug_shell_tx_state.h"
#include "airdap_swd.h"
#include "airdap_voltage_monitor.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

enum {
    DEBUG_VENDOR_INSTANCE = 1,
    SHELL_TASK_STACK_SIZE = 4096,
    SHELL_TASK_PRIORITY = 4,
    SHELL_READ_CHUNK = 64,
    SHELL_SESSION_START = 0x00,
    SHELL_SESSION_START_COLOR = 0x01,
    SHELL_SESSION_END = 0x04,
    SHELL_POLL_INTERVAL_MS = 10,
    SHELL_WRITE_TIMEOUT_MS = 100,
    RESTART_FLUSH_TIMEOUT_MS = 1000,
    SHELL_FORMAT_BUFFER_SIZE = 384,
    LOG_MIRROR_BUFFER_SIZE = 512,
    LOG_QUEUE_DEPTH = 4,
};

typedef int (*shell_command_handler_t)(const char *arguments);

typedef struct {
    const char *name;
    const char *help;
    shell_command_handler_t handler;
} shell_command_t;

typedef struct {
    uint32_t session_generation;
    size_t length;
    char data[LOG_MIRROR_BUFFER_SIZE];
} shell_log_message_t;

static int help_command(const char *arguments);
static int status_command(const char *arguments);
static int swd_idcode_command(const char *arguments);
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
        .name = "swd-idcode",
        .help = "Read the target DP IDCODE [clock_khz]",
        .handler = swd_idcode_command,
    },
    {
        .name = "restart",
        .help = "Restart the AirDAP firmware",
        .handler = restart_command,
    },
};

static SemaphoreHandle_t output_mutex;
static QueueHandle_t log_queue;
static atomic_bool session_active = ATOMIC_VAR_INIT(false);
static atomic_bool session_color_enabled = ATOMIC_VAR_INIT(false);
static _Atomic(uint32_t) session_generation = ATOMIC_VAR_INIT(0U);
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

static const char ansi_reset[] = "\x1b[0m";
static const char ansi_red[] = "\x1b[31m";
static const char ansi_green[] = "\x1b[32m";
static const char ansi_yellow[] = "\x1b[33m";
static const char ansi_cyan[] = "\x1b[36m";

static void shell_vprintf_styled(
    const char *style,
    const char *format,
    va_list arguments)
{
    char output[SHELL_FORMAT_BUFFER_SIZE];
    const bool styled = style != NULL && atomic_load(&session_color_enabled);
    const size_t style_length = styled ? strlen(style) : 0U;
    const size_t reset_length = styled ? sizeof(ansi_reset) - 1U : 0U;
    size_t length = 0U;

    if (style_length + reset_length + 1U >= sizeof(output)) {
        return;
    }
    if (styled) {
        memcpy(output, style, style_length);
        length = style_length;
    }

    const size_t content_capacity = sizeof(output) - length - reset_length;
    const int formatted = vsnprintf(
        output + length,
        content_capacity,
        format,
        arguments);

    if (formatted <= 0) {
        return;
    }
    const size_t content_length = (size_t) formatted < content_capacity
        ? (size_t) formatted
        : content_capacity - 1U;
    length += content_length;
    if (styled) {
        memcpy(output + length, ansi_reset, reset_length);
        length += reset_length;
    }
    shell_write(output, length, NULL);
}

static void shell_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    shell_vprintf_styled(NULL, format, arguments);
    va_end(arguments);
}

static void shell_printf_styled(const char *style, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    shell_vprintf_styled(style, format, arguments);
    va_end(arguments);
}

static int mirror_log_vprintf(const char *format, va_list arguments)
{
    va_list primary_arguments;
    va_copy(primary_arguments, arguments);
    const vprintf_like_t primary = atomic_load(&previous_log_vprintf);
    const int result = primary != NULL ? primary(format, primary_arguments) : 0;
    va_end(primary_arguments);

    if (!atomic_load(&session_active) || log_queue == NULL) {
        return result;
    }

    shell_log_message_t message = {
        .session_generation = atomic_load(&session_generation),
    };
    va_list mirror_arguments;
    va_copy(mirror_arguments, arguments);
    const int formatted = vsnprintf(
        message.data,
        sizeof(message.data),
        format,
        mirror_arguments);
    va_end(mirror_arguments);
    if (formatted > 0) {
        message.length = (size_t) formatted < sizeof(message.data)
            ? (size_t) formatted
            : sizeof(message.data) - 1U;
        if (atomic_load(&session_active) &&
            message.session_generation == atomic_load(&session_generation)) {
            (void) xQueueSend(log_queue, &message, 0);
        }
    }
    return result;
}

static void drain_log_queue(
    airdap_debug_shell_input_t *input,
    const airdap_debug_shell_input_callbacks_t *callbacks)
{
    shell_log_message_t message;

    for (size_t index = 0U; index < LOG_QUEUE_DEPTH; ++index) {
        if (!atomic_load(&session_active) ||
            xQueueReceive(log_queue, &message, 0) != pdTRUE) {
            return;
        }
        if (message.session_generation != atomic_load(&session_generation)) {
            continue;
        }
        airdap_debug_shell_input_write_background(
            input,
            message.data,
            message.length,
            callbacks);
    }
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

    shell_printf_styled(
        ansi_red,
        "Unrecognized command: %.*s\n",
        (int) command_length,
        line);
}

static const char *shell_complete(
    const char *prefix,
    size_t match_index,
    void *context)
{
    const size_t prefix_length = strlen(prefix);
    size_t current_match = 0U;

    (void) context;
    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        if (strncmp(commands[index].name, prefix, prefix_length) != 0) {
            continue;
        }
        if (current_match == match_index) {
            return commands[index].name;
        }
        ++current_match;
    }
    return NULL;
}

static int help_command(const char *arguments)
{
    if (*arguments != '\0') {
        shell_printf_styled(ansi_yellow, "usage: help\n");
        return 1;
    }

    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        if (atomic_load(&session_color_enabled)) {
            shell_printf(
                "%s%-8s%s %s\n",
                ansi_cyan,
                commands[index].name,
                ansi_reset,
                commands[index].help);
        } else {
            shell_printf("%-8s %s\n", commands[index].name, commands[index].help);
        }
    }
    return 0;
}

static int status_command(const char *arguments)
{
    if (*arguments != '\0') {
        shell_printf_styled(ansi_yellow, "usage: status\n");
        return 1;
    }

    airdap_voltage_reading_t voltage;
    const esp_err_t error = airdap_voltage_monitor_read(&voltage);
    if (error != ESP_OK) {
        shell_printf_styled(
            ansi_red,
            "status: voltage read failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }

    shell_printf_styled(
        ansi_green,
        "target_mv=%" PRIu32 " usb_vbus_mv=%" PRIu32
        " uptime_ms=%" PRId64 " free_heap=%" PRIu32 "\n",
        voltage.target_mv,
        voltage.usb_vbus_mv,
        esp_timer_get_time() / 1000,
        esp_get_free_heap_size());
    return 0;
}

static int shell_swd_set_clock(void *context, uint32_t clock_hz)
{
    (void) context;
    esp_err_t error = airdap_swd_set_clock(clock_hz);
    if (error != ESP_OK) {
        return (int) error;
    }
    return (int) airdap_swd_configure_bus(1U, false);
}

static int shell_swd_write_sequence(
    void *context,
    const uint8_t *data,
    size_t bit_count)
{
    (void) context;
    return (int) airdap_swd_write_sequence(data, bit_count);
}

static int shell_swd_read_sequence(
    void *context,
    uint8_t *data,
    size_t bit_count)
{
    (void) context;
    /*
     * The first sample captures ACK bit 0 on the edge ending turnaround. The
     * transaction then reads the remaining ACK, IDCODE, parity, and release.
     */
    return (int) airdap_swd_read_sequence(data, bit_count);
}

static int shell_swd_release(void *context)
{
    (void) context;
    return (int) airdap_swd_set_io_state(false);
}

static int swd_idcode_command(const char *arguments)
{
    static const airdap_debug_shell_swd_backend_t backend = {
        .context = NULL,
        .set_clock = shell_swd_set_clock,
        .write_sequence = shell_swd_write_sequence,
        .read_sequence = shell_swd_read_sequence,
        .release = shell_swd_release,
    };
    airdap_debug_shell_swd_probe_result_t result;
    const airdap_debug_shell_swd_probe_status_t status =
        airdap_debug_shell_swd_probe(arguments, &backend, &result);

    switch (status) {
    case AIRDAP_DEBUG_SHELL_SWD_PROBE_OK:
        shell_printf_styled(
            ansi_green,
            "swd-idcode: ok clock_khz=%" PRIu32 " idcode=0x%08" PRIX32 "\n",
            result.clock_khz,
            result.idcode);
        return 0;

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE:
        shell_printf_styled(ansi_yellow, "usage: swd-idcode [clock_khz]\n");
        return 1;

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE:
        shell_printf_styled(
            ansi_yellow,
            "swd-idcode: clock_khz must be %u..%u\n",
            AIRDAP_DEBUG_SHELL_SWD_MIN_CLOCK_KHZ,
            AIRDAP_DEBUG_SHELL_SWD_MAX_CLOCK_KHZ);
        return 1;

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_SET_CLOCK_FAILED:
        shell_printf_styled(
            ansi_red,
            "swd-idcode: set clock failed: %s\n",
            esp_err_to_name((esp_err_t) result.operation_error));
        break;

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED:
        shell_printf_styled(
            ansi_red,
            "swd-idcode: transfer failed clock_khz=%" PRIu32 " error=%s\n",
            result.clock_khz,
            esp_err_to_name((esp_err_t) result.operation_error));
        break;

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_RESPONSE_INVALID: {
        const char *ack_name = result.ack == AIRDAP_SWD_ACK_WAIT
            ? "WAIT"
            : result.ack == AIRDAP_SWD_ACK_FAULT ? "FAULT" : "invalid";
        shell_printf_styled(
            ansi_red,
            "swd-idcode: response failed clock_khz=%" PRIu32
            " ack=0x%X (%s) raw=0x%010" PRIX64 "\n",
            result.clock_khz,
            (unsigned int) result.ack,
            ack_name,
            result.response_bits);
        break;
    }

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_PARITY_ERROR:
        shell_printf_styled(
            ansi_red,
            "swd-idcode: parity error clock_khz=%" PRIu32
            " idcode=0x%08" PRIX32 " received=%u expected=%u"
            " raw=0x%010" PRIX64 "\n",
            result.clock_khz,
            result.idcode,
            (unsigned int) result.received_parity,
            (unsigned int) result.expected_parity,
            result.response_bits);
        break;

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_RELEASE_FAILED:
        shell_printf_styled(
            ansi_red,
            "swd-idcode: release failed after idcode=0x%08" PRIX32 ": %s\n",
            result.idcode,
            esp_err_to_name((esp_err_t) result.release_error));
        return 1;

    case AIRDAP_DEBUG_SHELL_SWD_PROBE_INVALID_BACKEND:
    default:
        shell_printf_styled(
            ansi_red,
            "swd-idcode: internal backend unavailable\n");
        return 1;
    }

    if (result.release_error != 0) {
        shell_printf_styled(
            ansi_red,
            "swd-idcode: SWDIO release also failed: %s\n",
            esp_err_to_name((esp_err_t) result.release_error));
    }
    return 1;
}

static int restart_command(const char *arguments)
{
    static const char acknowledgement[] = "Restarting AirDAP...\n";
    static const char colored_acknowledgement[] =
        "\x1b[33mRestarting AirDAP...\n\x1b[0m";

    if (*arguments != '\0') {
        shell_printf_styled(ansi_yellow, "usage: restart\n");
        return 1;
    }

    const char *output = atomic_load(&session_color_enabled)
        ? colored_acknowledgement
        : acknowledgement;
    if (!write_and_wait(
        output,
        strlen(output),
        pdMS_TO_TICKS(RESTART_FLUSH_TIMEOUT_MS))) {
        shell_printf_styled(
            ansi_red,
            "restart: acknowledgement transfer failed\n");
        return 1;
    }
    if (!atomic_load(&session_active) ||
        !tud_vendor_n_mounted(DEBUG_VENDOR_INSTANCE)) {
        return 1;
    }
    esp_restart();
    return 0;
}

static void start_session(
    airdap_debug_shell_input_t *input,
    airdap_debug_shell_input_callbacks_t *callbacks,
    bool color_enabled)
{
    airdap_debug_shell_disconnected();
    (void) atomic_fetch_add(&session_generation, 1U);
    (void) xQueueReset(log_queue);
    if (xSemaphoreTake(output_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&tx_state_lock);
    airdap_debug_shell_tx_state_connected(&tx_state);
    atomic_store(&session_color_enabled, color_enabled);
    atomic_store(&session_active, true);
    portEXIT_CRITICAL(&tx_state_lock);
    xSemaphoreGive(output_mutex);
    airdap_debug_shell_input_init(input);
    callbacks->color_enabled = color_enabled;
    shell_printf("\n");
    shell_printf_styled(ansi_cyan, "AirDAP debug shell\n");
    shell_printf("Tab completes or lists commands; arrows edit and recall history.\n");
    shell_printf("Type 'help' to list commands. Ctrl-] or Ctrl-D exits airdap-shell.\n");
    shell_printf_styled(ansi_cyan, "airdap> ");
}

static void end_session(void)
{
    airdap_debug_shell_disconnected();
}

static void shell_task(void *argument)
{
    (void) argument;
    static airdap_debug_shell_input_t input;
    airdap_debug_shell_input_callbacks_t callbacks = {
        .write = shell_write,
        .execute = shell_execute,
        .complete = shell_complete,
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

        if (atomic_load(&session_active)) {
            drain_log_queue(&input, &callbacks);
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
            if (data[index] == SHELL_SESSION_START ||
                data[index] == SHELL_SESSION_START_COLOR) {
                start_session(
                    &input,
                    &callbacks,
                    data[index] == SHELL_SESSION_START_COLOR);
            } else if (data[index] == SHELL_SESSION_END) {
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
        if (atomic_load(&session_active)) {
            drain_log_queue(&input, &callbacks);
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
    log_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(shell_log_message_t));
    if (log_queue == NULL) {
        vSemaphoreDelete(output_mutex);
        output_mutex = NULL;
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
        vQueueDelete(log_queue);
        log_queue = NULL;
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
    atomic_store(&session_color_enabled, false);

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
