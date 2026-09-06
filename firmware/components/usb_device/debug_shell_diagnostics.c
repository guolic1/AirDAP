#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "airdap_board.h"
#include "airdap_config_store.h"
#include "airdap_debug_shell_commands.h"
#include "airdap_debug_shell_config_status.h"
#include "airdap_debug_shell_diagnostics.h"
#include "airdap_debug_shell_identity.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#include "airdap_ota.h"
#include "airdap_voltage_monitor.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    TASK_DIAGNOSTIC_LIMIT = 48,
    TASK_NAME_COLUMN_WIDTH = configMAX_TASK_NAME_LEN,
    TASK_SAMPLE_MIN_MS = 100,
    TASK_SAMPLE_MAX_MS = 5000,
};

#if CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#define TASK_RUNTIME_FIELD "runtime_us"
#define TASK_RUNTIME_DELTA_FIELD "runtime_delta_us"
#else
#define TASK_RUNTIME_FIELD "runtime_ticks"
#define TASK_RUNTIME_DELTA_FIELD "runtime_delta_ticks"
#endif

typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    eTaskState state;
    UBaseType_t number;
    UBaseType_t priority;
    UBaseType_t base_priority;
    uint64_t stack_free_bytes;
    uint64_t runtime;
    BaseType_t core_id;
} task_diagnostic_t;

typedef struct {
    atomic_bool remote_suspended;
    atomic_bool release_remote;
    atomic_bool remote_released;
} task_snapshot_guard_t;

static TaskStatus_t task_status_buffer[TASK_DIAGNOSTIC_LIMIT];
static task_diagnostic_t task_diagnostic_buffers[2][TASK_DIAGNOSTIC_LIMIT];

/* TaskStatus_t names borrow storage from their TCBs. Keep both schedulers
 * suspended until those names have been copied so an idle task on the other
 * core cannot reclaim a deleted TCB after uxTaskGetSystemState() returns. */
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static void hold_remote_scheduler(void *context)
{
    task_snapshot_guard_t *guard = context;
    vTaskSuspendAll();
    atomic_store_explicit(
        &guard->remote_suspended,
        true,
        memory_order_release);
    while (!atomic_load_explicit(
        &guard->release_remote,
        memory_order_acquire)) {
    }
    /* Do not access the caller-owned guard after publishing this flag. */
    atomic_store_explicit(
        &guard->remote_released,
        true,
        memory_order_release);
    (void) xTaskResumeAll();
}
#endif

static esp_err_t task_snapshot_guard_enter(task_snapshot_guard_t *guard)
{
    atomic_init(&guard->remote_suspended, false);
    atomic_init(&guard->release_remote, false);
    atomic_init(&guard->remote_released, false);

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    const uint32_t current_core = (uint32_t) xPortGetCoreID();
    const uint32_t remote_core = current_core == 0U ? 1U : 0U;
    const esp_err_t error = esp_ipc_call(
        remote_core,
        hold_remote_scheduler,
        guard);
    if (error != ESP_OK) {
        return error;
    }
    while (!atomic_load_explicit(
        &guard->remote_suspended,
        memory_order_acquire)) {
    }
#endif

    vTaskSuspendAll();
    return ESP_OK;
}

static void task_snapshot_guard_exit(task_snapshot_guard_t *guard)
{
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    atomic_store_explicit(
        &guard->release_remote,
        true,
        memory_order_release);
    while (!atomic_load_explicit(
        &guard->remote_released,
        memory_order_acquire)) {
    }
#else
    (void) guard;
#endif
    (void) xTaskResumeAll();
}

static int usage_error(
    const airdap_debug_shell_invocation_t *invocation,
    const char *usage)
{
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_WARNING,
        "usage: %s\n",
        usage);
    return 1;
}

static bool has_no_arguments(const char *arguments)
{
    return arguments != NULL && arguments[0] == '\0';
}

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:
        return "esp32";
    case CHIP_ESP32S2:
        return "esp32s2";
    case CHIP_ESP32S3:
        return "esp32s3";
    case CHIP_ESP32C3:
        return "esp32c3";
    case CHIP_ESP32C2:
        return "esp32c2";
    case CHIP_ESP32C6:
        return "esp32c6";
    case CHIP_ESP32H2:
        return "esp32h2";
    case CHIP_ESP32P4:
        return "esp32p4";
    case CHIP_ESP32C61:
        return "esp32c61";
    case CHIP_ESP32C5:
        return "esp32c5";
    case CHIP_ESP32H21:
        return "esp32h21";
    case CHIP_ESP32H4:
        return "esp32h4";
    case CHIP_ESP32S31:
        return "esp32s31";
    case CHIP_POSIX_LINUX:
        return "posix-linux";
    default:
        return "unknown";
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external-pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
        return "task-watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power-glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu-lockup";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
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
        return "unknown";
    }
}

static const char *provisioning_state_name(
    airdap_provisioning_state_t state)
{
    switch (state) {
    case AIRDAP_PROVISIONING_IDLE:
        return "idle";
    case AIRDAP_PROVISIONING_ACTIVE:
        return "active";
    case AIRDAP_PROVISIONING_SUCCEEDED:
        return "succeeded";
    case AIRDAP_PROVISIONING_TIMED_OUT:
        return "timed-out";
    default:
        return "unknown";
    }
}

static const char *ota_session_state_name(airdap_mode_ota_state_t state)
{
    switch (state) {
    case AIRDAP_OTA_IDLE:
        return "idle";
    case AIRDAP_OTA_RECEIVING:
        return "receiving";
    case AIRDAP_OTA_READY_TO_REBOOT:
        return "ready-to-reboot";
    default:
        return "unknown";
    }
}

static const char *dap_owner_name(airdap_dap_owner_t owner)
{
    switch (owner) {
    case AIRDAP_DAP_OWNER_NONE:
        return "none";
    case AIRDAP_DAP_OWNER_USB:
        return "usb";
    case AIRDAP_DAP_OWNER_NETWORK:
        return "network";
    case AIRDAP_DAP_OWNER_DIAGNOSTIC:
        return "diagnostic";
    default:
        return "unknown";
    }
}

static const char *ota_image_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending-verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "undefined";
    }
}

static const char *task_state_name(eTaskState state)
{
    switch (state) {
    case eRunning:
        return "running";
    case eReady:
        return "ready";
    case eBlocked:
        return "blocked";
    case eSuspended:
        return "suspended";
    case eDeleted:
        return "deleted";
    case eInvalid:
    default:
        return "invalid";
    }
}

static int system_info_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (!has_no_arguments(arguments)) {
        return usage_error(invocation, "system-info");
    }

    const airdap_device_identity_t *identity = airdap_device_identity_get();
    if (identity == NULL) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "system-info: device identity unavailable\n");
        return 1;
    }

    char identity_output[AIRDAP_DEBUG_SHELL_IDENTITY_OUTPUT_SIZE];
    if (!airdap_debug_shell_identity_format(
            identity,
            identity_output,
            sizeof(identity_output))) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "system-info: identity formatting failed\n");
        return 1;
    }

    esp_chip_info_t chip_info = {0};
    esp_chip_info(&chip_info);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "%s",
        identity_output);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "idf_version=%s uptime_ms=%" PRId64 "\n",
        esp_get_idf_version(),
        esp_timer_get_time() / INT64_C(1000));
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "chip_model=%s chip_revision=%u.%u chip_cores=%u "
        "chip_features=0x%08" PRIX32 " reset_reason=%s\n",
        chip_model_name(chip_info.model),
        (unsigned int) (chip_info.revision / 100U),
        (unsigned int) (chip_info.revision % 100U),
        (unsigned int) chip_info.cores,
        chip_info.features,
        reset_reason_name(esp_reset_reason()));
    return 0;
}

static void print_heap_line(
    const airdap_debug_shell_invocation_t *invocation,
    const char *name,
    uint32_t capabilities)
{
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "%s total=%zu free=%zu min_free=%zu largest=%zu\n",
        name,
        heap_caps_get_total_size(capabilities),
        heap_caps_get_free_size(capabilities),
        heap_caps_get_minimum_free_size(capabilities),
        heap_caps_get_largest_free_block(capabilities));
}

static int memory_info_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (!has_no_arguments(arguments)) {
        return usage_error(invocation, "memory-info");
    }

    print_heap_line(invocation, "default", MALLOC_CAP_DEFAULT);
    print_heap_line(invocation, "internal", MALLOC_CAP_INTERNAL);
    print_heap_line(invocation, "dma", MALLOC_CAP_DMA);
    print_heap_line(invocation, "spiram", MALLOC_CAP_SPIRAM);
    return 0;
}

static bool read_mode_snapshot(
    const airdap_debug_shell_invocation_t *invocation,
    const char *command_name,
    airdap_mode_snapshot_t *snapshot)
{
    const airdap_mode_state_result_t result =
        airdap_mode_state_get(snapshot);
    if (result == AIRDAP_MODE_STATE_OK) {
        return true;
    }

    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_ERROR,
        "%s: mode state read failed: %d\n",
        command_name,
        (int) result);
    return false;
}

static int mode_status_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (!has_no_arguments(arguments)) {
        return usage_error(invocation, "mode-status");
    }

    airdap_config_status_t config_status;
    const esp_err_t config_error =
        airdap_config_store_get_status(&config_status);
    if (config_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "mode-status: config status read failed: %s\n",
            esp_err_to_name(config_error));
        return 1;
    }
    char config_output[AIRDAP_DEBUG_SHELL_CONFIG_STATUS_OUTPUT_SIZE];
    if (!airdap_debug_shell_config_status_format(
            &config_status,
            config_output,
            sizeof(config_output))) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "mode-status: config status formatting failed\n");
        return 1;
    }

    airdap_mode_snapshot_t snapshot;
    if (!read_mode_snapshot(invocation, "mode-status", &snapshot)) {
        return 1;
    }
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "%s",
        config_output);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "usb=%s wifi=%s provisioning=%s ota=%s dap_owner=%s\n",
        snapshot.usb_present ? "present" : "absent",
        wifi_state_name(snapshot.wifi),
        provisioning_state_name(snapshot.provisioning),
        ota_session_state_name(snapshot.ota),
        dap_owner_name(snapshot.dap_owner));
    return 0;
}

static int ota_status_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (!has_no_arguments(arguments)) {
        return usage_error(invocation, "ota-status");
    }

    airdap_ota_info_t info;
    const airdap_ota_status_t info_result = airdap_ota_get_info(&info);
    if (info_result != AIRDAP_OTA_STATUS_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "ota-status: OTA info read failed: %u\n",
            (unsigned int) info_result);
        return 1;
    }
    airdap_mode_snapshot_t snapshot;
    if (!read_mode_snapshot(invocation, "ota-status", &snapshot)) {
        return 1;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (running == NULL || boot == NULL) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "ota-status: partition metadata unavailable\n");
        return 1;
    }

    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_error =
        esp_ota_get_state_partition(running, &image_state);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "running_version=%s protocol_version=%u max_image_size=%" PRIu32
        " rollback=%s session_state=%s\n",
        info.running_version,
        (unsigned int) info.protocol_version,
        info.max_image_size,
        (info.flags & AIRDAP_OTA_FLAG_ROLLBACK) != 0U
            ? "supported"
            : "unsupported",
        ota_session_state_name(snapshot.ota));
    if (state_error == ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "running_partition=%s address=0x%08" PRIX32
            " size=%" PRIu32 " image_state=%s\n",
            running->label,
            running->address,
            running->size,
            ota_image_state_name(image_state));
    } else {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "running_partition=%s address=0x%08" PRIX32
            " size=%" PRIu32 " image_state=unavailable error=%s\n",
            running->label,
            running->address,
            running->size,
            esp_err_to_name(state_error));
    }
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "boot_partition=%s address=0x%08" PRIX32 " size=%" PRIu32 "\n",
        boot->label,
        boot->address,
        boot->size);
    return 0;
}

static int target_status_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (!has_no_arguments(arguments)) {
        return usage_error(invocation, "target-status");
    }

    bool power_active = false;
    esp_err_t error = airdap_target_power_get_active(&power_active);
    if (error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "target-status: power status read failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }

    airdap_voltage_reading_t voltage;
    error = airdap_voltage_monitor_read(&voltage);
    if (error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "target-status: voltage read failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }

    airdap_mode_snapshot_t snapshot;
    if (!read_mode_snapshot(invocation, "target-status", &snapshot)) {
        return 1;
    }
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "power_active=%s target_mv=%" PRIu32 " usb_vbus_mv=%" PRIu32
        " dap_owner=%s\n",
        power_active ? "yes" : "no",
        voltage.target_mv,
        voltage.usb_vbus_mv,
        dap_owner_name(snapshot.dap_owner));
    return 0;
}

static uint32_t cpu_basis_points(uint64_t runtime, uint64_t total_runtime)
{
    if (total_runtime == 0U || CONFIG_FREERTOS_NUMBER_OF_CORES == 0) {
        return 0U;
    }
    uint64_t capacity_runtime = total_runtime;
    if (capacity_runtime <=
        UINT64_MAX / (uint64_t) CONFIG_FREERTOS_NUMBER_OF_CORES) {
        capacity_runtime *= (uint64_t) CONFIG_FREERTOS_NUMBER_OF_CORES;
    } else {
        capacity_runtime = UINT64_MAX;
    }
    if (runtime >= capacity_runtime) {
        return UINT32_C(10000);
    }
    if (runtime <= UINT64_MAX / UINT64_C(10000)) {
        return (uint32_t) (
            runtime * UINT64_C(10000) / capacity_runtime);
    }
    return (uint32_t) (
        runtime / (capacity_runtime / UINT64_C(10000)));
}

static void copy_task_diagnostic(
    task_diagnostic_t *destination,
    const TaskStatus_t *source)
{
    (void) snprintf(
        destination->name,
        sizeof(destination->name),
        "%s",
        source->pcTaskName != NULL ? source->pcTaskName : "<unnamed>");
    destination->state = source->eCurrentState;
    destination->number = source->xTaskNumber;
    destination->priority = source->uxCurrentPriority;
    destination->base_priority = source->uxBasePriority;
    destination->stack_free_bytes =
        (uint64_t) source->usStackHighWaterMark * sizeof(StackType_t);
    destination->runtime = (uint64_t) source->ulRunTimeCounter;
#if configTASKLIST_INCLUDE_COREID == 1
    destination->core_id = source->xCoreID;
#else
    destination->core_id = tskNO_AFFINITY;
#endif
}

static void sort_tasks_by_runtime(task_diagnostic_t *tasks, size_t count)
{
    for (size_t index = 1U; index < count; ++index) {
        const task_diagnostic_t current = tasks[index];
        size_t insertion = index;
        while (insertion > 0U &&
               (tasks[insertion - 1U].runtime < current.runtime ||
                (tasks[insertion - 1U].runtime == current.runtime &&
                 strcmp(tasks[insertion - 1U].name, current.name) > 0))) {
            tasks[insertion] = tasks[insertion - 1U];
            --insertion;
        }
        tasks[insertion] = current;
    }
}

static bool parse_task_interval(
    const char *arguments,
    bool *sampled,
    uint32_t *interval_ms)
{
    if (arguments == NULL || sampled == NULL || interval_ms == NULL) {
        return false;
    }
    if (arguments[0] == '\0') {
        *sampled = false;
        *interval_ms = 0U;
        return true;
    }

    static const char prefix[] = "--interval ";
    if (strncmp(arguments, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    const char *cursor = arguments + sizeof(prefix) - 1U;
    if (*cursor == '\0') {
        return false;
    }

    uint32_t value = 0U;
    while (*cursor != '\0') {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t) (*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        ++cursor;
    }
    if (value < TASK_SAMPLE_MIN_MS || value > TASK_SAMPLE_MAX_MS) {
        return false;
    }

    *sampled = true;
    *interval_ms = value;
    return true;
}

static bool capture_task_snapshot(
    const airdap_debug_shell_invocation_t *invocation,
    task_diagnostic_t tasks[TASK_DIAGNOSTIC_LIMIT],
    UBaseType_t *task_count,
    uint64_t *total_runtime)
{
    task_snapshot_guard_t guard;
    const esp_err_t guard_error = task_snapshot_guard_enter(&guard);
    if (guard_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "tasks: unable to suspend both schedulers: %s\n",
            esp_err_to_name(guard_error));
        return false;
    }

    const UBaseType_t reported_count = uxTaskGetNumberOfTasks();
    if (reported_count > TASK_DIAGNOSTIC_LIMIT) {
        task_snapshot_guard_exit(&guard);
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "tasks: task count %u exceeds diagnostic limit %u\n",
            (unsigned int) reported_count,
            (unsigned int) TASK_DIAGNOSTIC_LIMIT);
        return false;
    }

    configRUN_TIME_COUNTER_TYPE captured_total_runtime = 0;
    const UBaseType_t captured_count = uxTaskGetSystemState(
        task_status_buffer,
        TASK_DIAGNOSTIC_LIMIT,
        &captured_total_runtime);
    if (captured_count == 0U || captured_count > TASK_DIAGNOSTIC_LIMIT) {
        task_snapshot_guard_exit(&guard);
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "tasks: unable to capture a stable task snapshot\n");
        return false;
    }

    for (UBaseType_t index = 0U; index < captured_count; ++index) {
        copy_task_diagnostic(&tasks[index], &task_status_buffer[index]);
    }
    task_snapshot_guard_exit(&guard);
    *task_count = captured_count;
    *total_runtime = (uint64_t) captured_total_runtime;
    return true;
}

static uint64_t task_runtime_delta(
    const task_diagnostic_t *task,
    const task_diagnostic_t *previous,
    UBaseType_t previous_count)
{
    for (UBaseType_t index = 0U; index < previous_count; ++index) {
        if (previous[index].number == task->number &&
            strcmp(previous[index].name, task->name) == 0) {
            return task->runtime >= previous[index].runtime
                ? task->runtime - previous[index].runtime
                : 0U;
        }
    }
    return 0U;
}

static void convert_to_task_runtime_deltas(
    task_diagnostic_t *current,
    UBaseType_t current_count,
    const task_diagnostic_t *previous,
    UBaseType_t previous_count)
{
    for (UBaseType_t index = 0U; index < current_count; ++index) {
        current[index].runtime = task_runtime_delta(
            &current[index],
            previous,
            previous_count);
    }
}

static void print_task_table(
    const airdap_debug_shell_invocation_t *invocation,
    task_diagnostic_t *tasks,
    UBaseType_t task_count,
    uint64_t total_runtime,
    bool sampled,
    uint32_t interval_ms)
{
    sort_tasks_by_runtime(tasks, task_count);
    if (sampled) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "tasks=%u sample_ms=%" PRIu32 " total_delta_"
            TASK_RUNTIME_FIELD "=%" PRIu64 " cpu_capacity_cores=%u\n",
            (unsigned int) task_count,
            interval_ms,
            total_runtime,
            (unsigned int) CONFIG_FREERTOS_NUMBER_OF_CORES);
    } else {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "tasks=%u total_" TASK_RUNTIME_FIELD "=%" PRIu64
            " cpu_capacity_cores=%u\n",
            (unsigned int) task_count,
            total_runtime,
            (unsigned int) CONFIG_FREERTOS_NUMBER_OF_CORES);
    }
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "%-10s %-*s %-9s %-8s %-10s %-13s %-16s %-20s %s\n",
        "number",
        TASK_NAME_COLUMN_WIDTH,
        "name",
        "state",
        "affinity",
        "priority",
        "base_priority",
        "stack_free_bytes",
        sampled ? TASK_RUNTIME_DELTA_FIELD : TASK_RUNTIME_FIELD,
        "cpu_pct");
    for (UBaseType_t index = 0U; index < task_count; ++index) {
        const task_diagnostic_t *task = &tasks[index];
        const uint32_t cpu = cpu_basis_points(task->runtime, total_runtime);
        char affinity[12];
        if (task->core_id == tskNO_AFFINITY) {
            (void) snprintf(affinity, sizeof(affinity), "any");
        } else {
            (void) snprintf(
                affinity,
                sizeof(affinity),
                "%d",
                (int) task->core_id);
        }
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "%-10u %-*s %-9s %-8s %-10u %-13u %-16" PRIu64
            " %-20" PRIu64 " %" PRIu32 ".%02" PRIu32 "\n",
            (unsigned int) task->number,
            TASK_NAME_COLUMN_WIDTH,
            task->name,
            task_state_name(task->state),
            affinity,
            (unsigned int) task->priority,
            (unsigned int) task->base_priority,
            task->stack_free_bytes,
            task->runtime,
            cpu / 100U,
            cpu % 100U);
    }
}

static int tasks_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    bool sampled;
    uint32_t interval_ms;
    if (!parse_task_interval(arguments, &sampled, &interval_ms)) {
        return usage_error(invocation, "tasks [--interval <ms>]");
    }

    UBaseType_t first_count;
    uint64_t first_total_runtime;
    if (!capture_task_snapshot(
            invocation,
            task_diagnostic_buffers[0],
            &first_count,
            &first_total_runtime)) {
        return 1;
    }

    task_diagnostic_t *output_tasks = task_diagnostic_buffers[0];
    UBaseType_t output_count = first_count;
    uint64_t output_total_runtime = first_total_runtime;
    if (sampled) {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        uint64_t second_total_runtime;
        if (!capture_task_snapshot(
                invocation,
                task_diagnostic_buffers[1],
                &output_count,
                &second_total_runtime)) {
            return 1;
        }
        output_tasks = task_diagnostic_buffers[1];
        convert_to_task_runtime_deltas(
            output_tasks,
            output_count,
            task_diagnostic_buffers[0],
            first_count);
        output_total_runtime = second_total_runtime >= first_total_runtime
            ? second_total_runtime - first_total_runtime
            : 0U;
    }

    print_task_table(
        invocation,
        output_tasks,
        output_count,
        output_total_runtime,
        sampled,
        interval_ms);
    return 0;
}

static const airdap_debug_shell_command_t diagnostic_commands[] = {
    {
        .name = "system-info",
        .usage = "system-info",
        .summary = "Show device identity, firmware, and chip details",
        .details =
            "Reports the USB serial, device ID, UUID, firmware and protocol "
            "versions, capabilities, ESP-IDF version, chip identity, uptime, "
            "and the previous reset reason without changing device state.",
        .handler = system_info_command,
    },
    {
        .name = "memory-info",
        .usage = "memory-info",
        .summary = "Show capability-specific heap statistics",
        .details =
            "Reports total, free, historical minimum, and largest blocks for "
            "default, internal, DMA, and SPI RAM heaps; categories overlap.",
        .handler = memory_info_command,
    },
    {
        .name = "mode-status",
        .usage = "mode-status",
        .summary = "Show configuration and runtime mode state",
        .details =
            "Reports safe persistent configuration status plus USB, Wi-Fi, "
            "provisioning, OTA, and DAP-owner runtime state. Credential and "
            "authentication material are never displayed.",
        .handler = mode_status_command,
    },
    {
        .name = "ota-status",
        .usage = "ota-status",
        .summary = "Show OTA runtime and partition metadata",
        .details =
            "Reports the running version, transfer state, rollback capability, "
            "and current running and configured boot partitions.",
        .handler = ota_status_command,
    },
    {
        .name = "target-status",
        .usage = "target-status",
        .summary = "Show target power, voltage, and DAP ownership",
        .details =
            "Reads the target power/status line, calibrated target and USB "
            "voltages, and the current DAP owner without driving target pins.",
        .handler = target_status_command,
    },
    {
        .name = "tasks",
        .usage = "tasks [--interval <ms>]",
        .summary = "Analyze FreeRTOS task state, stack, and CPU time",
        .details =
            "Takes a bounded task snapshot and sorts it by runtime. With "
            "--interval, samples 100-5000 ms and reports recent deltas; without "
            "it, CPU time is cumulative since boot. Core values are affinities, "
            "and stack high-water marks are free bytes. Each snapshot briefly "
            "suspends scheduling on both cores while task names are copied.",
        .handler = tasks_command,
    },
};

bool airdap_debug_shell_register_diagnostic_commands(
    airdap_debug_shell_command_registry_t *registry)
{
    for (size_t index = 0U;
         index < sizeof(diagnostic_commands) / sizeof(diagnostic_commands[0]);
         ++index) {
        if (airdap_debug_shell_command_register(
                registry,
                &diagnostic_commands[index]) !=
            AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED) {
            return false;
        }
    }
    return true;
}
