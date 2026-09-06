#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "airdap_board.h"
#include "airdap_config_store.h"
#include "airdap_debug_shell_commands.h"
#include "airdap_debug_shell_diagnostics.h"
#include "airdap_debug_shell_core_commands.h"
#include "airdap_debug_shell_service_diagnostics.h"
#include "airdap_dap_service.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#include "airdap_ota.h"
#include "airdap_voltage_monitor.h"
#include "airdap_wifi_manager.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/task.h"

typedef struct {
    char text[8192];
    size_t length;
    airdap_debug_shell_style_t last_style;
} captured_output_t;

static airdap_device_identity_t identity;
static airdap_config_status_t config_status;
static esp_err_t config_status_result;
static airdap_mode_snapshot_t mode_snapshot;
static airdap_ota_info_t ota_info;
static esp_partition_t running_partition;
static esp_partition_t boot_partition;
static esp_ota_img_states_t running_image_state;
static bool target_power_active;
static airdap_voltage_reading_t voltage;
static airdap_dap_service_stats_t dap_stats;
static airdap_wifi_manager_info_t wifi_info;
static esp_err_t wifi_info_result;
static TaskStatus_t task_statuses[3];
static configRUN_TIME_COUNTER_TYPE task_total_runtime;
static UBaseType_t reported_task_count;
static UBaseType_t captured_task_count;
static esp_err_t ipc_result;
static esp_ipc_func_t ipc_function;
static void *ipc_context;
static atomic_bool ipc_started;
static atomic_uint suspended_scheduler_count;
static atomic_uint maximum_suspended_scheduler_count;
static bool advance_task_runtime_on_delay;
static TickType_t last_delay_ticks;

static void capture_vprintf(
    airdap_debug_shell_style_t style,
    const char *format,
    va_list arguments,
    void *context)
{
    captured_output_t *captured = context;
    captured->last_style = style;
    const int formatted = vsnprintf(
        captured->text + captured->length,
        sizeof(captured->text) - captured->length,
        format,
        arguments);
    assert(formatted >= 0);
    assert((size_t) formatted < sizeof(captured->text) - captured->length);
    captured->length += (size_t) formatted;
}

static int run_command(
    const airdap_debug_shell_command_registry_t *registry,
    const char *name,
    const char *arguments,
    captured_output_t *captured)
{
    const airdap_debug_shell_command_t *command =
        airdap_debug_shell_command_find(registry, name, strlen(name));
    assert(command != NULL);
    const airdap_debug_shell_invocation_t invocation = {
        .vprintf = capture_vprintf,
        .output_context = captured,
    };
    return command->handler(arguments, &invocation, command->context);
}

int airdap_debug_shell_help_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) arguments;
    (void) invocation;
    (void) context;
    return 0;
}

int airdap_debug_shell_wifi_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    return airdap_debug_shell_help_command(arguments, invocation, context);
}

int airdap_debug_shell_swd_idcode_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    return airdap_debug_shell_help_command(arguments, invocation, context);
}

int airdap_debug_shell_restart_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    return airdap_debug_shell_help_command(arguments, invocation, context);
}

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return &identity;
}

esp_err_t airdap_config_store_get_status(airdap_config_status_t *status)
{
    if (config_status_result == ESP_OK) {
        *status = config_status;
    }
    return config_status_result;
}

void esp_chip_info(esp_chip_info_t *info)
{
    *info = (esp_chip_info_t) {
        .model = CHIP_ESP32S3,
        .features = UINT32_C(0x91),
        .revision = 203U,
        .cores = 2U,
    };
}

const char *esp_get_idf_version(void)
{
    return "v6.1";
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_SW;
}

int64_t esp_timer_get_time(void)
{
    return INT64_C(1234567000);
}

static size_t heap_value(uint32_t caps, size_t offset)
{
    if (caps == MALLOC_CAP_DEFAULT) {
        return 100000U + offset;
    }
    if (caps == MALLOC_CAP_INTERNAL) {
        return 200000U + offset;
    }
    if (caps == MALLOC_CAP_DMA) {
        return 300000U + offset;
    }
    assert(caps == MALLOC_CAP_SPIRAM);
    return 400000U + offset;
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    return heap_value(caps, 1U);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    return heap_value(caps, 2U);
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    return heap_value(caps, 3U);
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return heap_value(caps, 4U);
}

airdap_mode_state_result_t airdap_mode_state_get(
    airdap_mode_snapshot_t *snapshot)
{
    *snapshot = mode_snapshot;
    return AIRDAP_MODE_STATE_OK;
}

airdap_ota_status_t airdap_ota_get_info(airdap_ota_info_t *info)
{
    *info = ota_info;
    return AIRDAP_OTA_STATUS_OK;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return &running_partition;
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return &boot_partition;
}

esp_err_t esp_ota_get_state_partition(
    const esp_partition_t *partition,
    esp_ota_img_states_t *state)
{
    assert(partition == &running_partition);
    *state = running_image_state;
    return ESP_OK;
}

esp_err_t airdap_target_power_get_active(bool *active)
{
    *active = target_power_active;
    return ESP_OK;
}

esp_err_t airdap_voltage_monitor_read(airdap_voltage_reading_t *reading)
{
    *reading = voltage;
    return ESP_OK;
}

void airdap_dap_service_get_stats(airdap_dap_service_stats_t *stats)
{
    *stats = dap_stats;
}

esp_err_t airdap_wifi_manager_get_info(airdap_wifi_manager_info_t *info)
{
    if (wifi_info_result == ESP_OK) {
        *info = wifi_info;
    }
    return wifi_info_result;
}

UBaseType_t uxTaskGetNumberOfTasks(void)
{
    assert(atomic_load(&suspended_scheduler_count) == 2U);
    return reported_task_count;
}

UBaseType_t uxTaskGetSystemState(
    TaskStatus_t *tasks,
    UBaseType_t capacity,
    configRUN_TIME_COUNTER_TYPE *total_runtime)
{
    assert(atomic_load(&suspended_scheduler_count) == 2U);
    assert(capacity >= captured_task_count);
    if (captured_task_count > 0U) {
        assert(captured_task_count <= 3U);
        memcpy(
            tasks,
            task_statuses,
            captured_task_count * sizeof(task_statuses[0]));
    }
    *total_runtime = task_total_runtime;
    return captured_task_count;
}

BaseType_t xPortGetCoreID(void)
{
    return 1;
}

void vTaskSuspendAll(void)
{
    const unsigned int count =
        atomic_fetch_add(&suspended_scheduler_count, 1U) + 1U;
    unsigned int maximum = atomic_load(&maximum_suspended_scheduler_count);
    while (maximum < count && !atomic_compare_exchange_weak(
        &maximum_suspended_scheduler_count,
        &maximum,
        count)) {
    }
}

BaseType_t xTaskResumeAll(void)
{
    assert(atomic_load(&suspended_scheduler_count) > 0U);
    (void) atomic_fetch_sub(&suspended_scheduler_count, 1U);
    return pdFALSE;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    assert(atomic_load(&suspended_scheduler_count) == 0U);
    last_delay_ticks = ticks_to_delay;
    if (!advance_task_runtime_on_delay) {
        return;
    }

    const TaskStatus_t first = task_statuses[0];
    task_statuses[0] = task_statuses[2];
    task_statuses[2] = first;
    task_statuses[0].ulRunTimeCounter += UINT64_C(50000);
    task_statuses[1].ulRunTimeCounter += UINT64_C(100000);
    task_statuses[2].ulRunTimeCounter += UINT64_C(50000);
    task_total_runtime += UINT64_C(100000);
}

static void *run_ipc_callback(void *context)
{
    (void) context;
    const esp_ipc_func_t function = ipc_function;
    void *const function_context = ipc_context;
    atomic_store(&ipc_started, true);
    function(function_context);
    return NULL;
}

esp_err_t esp_ipc_call(
    uint32_t core_id,
    esp_ipc_func_t function,
    void *context)
{
    assert(core_id == 0U);
    if (ipc_result != ESP_OK) {
        return ipc_result;
    }
    ipc_function = function;
    ipc_context = context;
    atomic_store(&ipc_started, false);

    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_ipc_callback, NULL) == 0);
    assert(pthread_detach(thread) == 0);
    while (!atomic_load(&ipc_started)) {
    }
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

static void set_up(void)
{
    atomic_init(&ipc_started, false);
    atomic_init(&suspended_scheduler_count, 0U);
    atomic_init(&maximum_suspended_scheduler_count, 0U);
    reported_task_count = 3U;
    captured_task_count = 3U;
    ipc_result = ESP_OK;
    advance_task_runtime_on_delay = false;
    last_delay_ticks = 0U;
    memset(&identity, 0, sizeof(identity));
    (void) snprintf(
        identity.usb_serial,
        sizeof(identity.usb_serial),
        "ADP-001122334455");
    (void) snprintf(
        identity.device_id,
        sizeof(identity.device_id),
        "ADP-001122334455");
    for (size_t index = 0U; index < sizeof(identity.uuid); ++index) {
        identity.uuid[index] = (uint8_t) index;
    }
    identity.firmware_version = "abc1234";
    identity.protocol_version = 1U;
    identity.capabilities = UINT32_C(0x1F);
    config_status = (airdap_config_status_t) {
        .schema_version = 1U,
        .provisioned = true,
    };
    config_status_result = ESP_OK;

    mode_snapshot = (airdap_mode_snapshot_t) {
        .usb_present = true,
        .wifi = AIRDAP_WIFI_ONLINE,
        .provisioning = AIRDAP_PROVISIONING_ACTIVE,
        .ota = AIRDAP_OTA_RECEIVING,
        .dap_owner = AIRDAP_DAP_OWNER_USB,
    };
    ota_info = (airdap_ota_info_t) {
        .max_image_size = 0x3F0000U,
        .protocol_version = 1U,
        .flags = AIRDAP_OTA_FLAG_ROLLBACK,
        .running_version = "abc1234",
    };
    running_partition = (esp_partition_t) {
        .address = 0x10000U,
        .size = 0x3F0000U,
        .label = "ota_0",
    };
    boot_partition = (esp_partition_t) {
        .address = 0x400000U,
        .size = 0x3F0000U,
        .label = "ota_1",
    };
    running_image_state = ESP_OTA_IMG_VALID;
    target_power_active = true;
    voltage = (airdap_voltage_reading_t) {
        .target_mv = 3301U,
        .usb_vbus_mv = 4998U,
    };
    dap_stats = (airdap_dap_service_stats_t) {
        .requests_accepted = 101U,
        .requests_processed = 99U,
        .responses_delivered = 97U,
        .queue_full = 2U,
        .timed_out = 3U,
        .stale_requests = 4U,
        .stale_responses = 5U,
        .delivery_failures = 6U,
    };
    wifi_info = (airdap_wifi_manager_info_t) {
        .started = true,
        .has_configuration = true,
        .link_connected = true,
        .provisioning_suspended = false,
        .last_failure = AIRDAP_WIFI_MANAGER_FAILURE_NONE,
        .last_disconnect_reason = 200U,
        .retry_delay_ms = 0U,
        .retry_scheduled = false,
        .ipv4_available = true,
        .ipv4_address = {192U, 168U, 4U, 20U},
        .ipv4_netmask = {255U, 255U, 255U, 0U},
        .ipv4_gateway = {192U, 168U, 4U, 1U},
        .ap_available = true,
        .rssi_dbm = -47,
        .channel = 6U,
    };
    wifi_info_result = ESP_OK;
    task_statuses[0] = (TaskStatus_t) {
        .pcTaskName = "wifi",
        .xTaskNumber = 8U,
        .eCurrentState = eBlocked,
        .uxCurrentPriority = 5U,
        .uxBasePriority = 4U,
        .ulRunTimeCounter = UINT64_C(500000),
        .usStackHighWaterMark = 768U,
        .xCoreID = 0,
    };
    task_statuses[1] = (TaskStatus_t) {
        .pcTaskName = "IDLE0",
        .xTaskNumber = 1U,
        .eCurrentState = eReady,
        .uxCurrentPriority = 0U,
        .uxBasePriority = 0U,
        .ulRunTimeCounter = UINT64_C(1200000),
        .usStackHighWaterMark = 512U,
        .xCoreID = 0,
    };
    task_statuses[2] = (TaskStatus_t) {
        .pcTaskName = "task-name-12345",
        .xTaskNumber = 12U,
        .eCurrentState = eRunning,
        .uxCurrentPriority = 4U,
        .uxBasePriority = 4U,
        .ulRunTimeCounter = UINT64_C(100000),
        .usStackHighWaterMark = 384U,
        .xCoreID = tskNO_AFFINITY,
    };
    task_total_runtime = UINT64_C(1000000);
}

static void wait_for_schedulers_to_resume(void)
{
    while (atomic_load(&suspended_scheduler_count) != 0U) {
    }
}

static void test_registers_diagnostics_with_detailed_metadata(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);

    assert(airdap_debug_shell_register_diagnostic_commands(&registry));
    assert(airdap_debug_shell_command_count(&registry) == 6U);
    static const char *const expected[] = {
        "system-info",
        "memory-info",
        "mode-status",
        "ota-status",
        "target-status",
        "tasks",
    };
    for (size_t index = 0U; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        const airdap_debug_shell_command_t *command =
            airdap_debug_shell_command_at(&registry, index);
        assert(command != NULL);
        assert(strcmp(command->name, expected[index]) == 0);
        assert(command->usage != NULL && command->usage[0] != '\0');
        assert(command->summary != NULL && command->summary[0] != '\0');
        assert(command->details != NULL && command->details[0] != '\0');
    }
    assert(!airdap_debug_shell_register_diagnostic_commands(&registry));
}

static void test_registers_complete_shell_without_legacy_duplicates(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);

    assert(airdap_debug_shell_register_core_commands(&registry));
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));
    assert(airdap_debug_shell_register_service_diagnostic_commands(&registry));
    static const char *const expected[] = {
        "help",
        "wifi",
        "swd-idcode",
        "restart",
        "system-info",
        "memory-info",
        "mode-status",
        "ota-status",
        "target-status",
        "tasks",
        "dap-stats",
        "network-info",
    };
    assert(airdap_debug_shell_command_count(&registry) ==
        sizeof(expected) / sizeof(expected[0]));
    for (size_t index = 0U; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        const airdap_debug_shell_command_t *command =
            airdap_debug_shell_command_at(&registry, index);
        assert(command != NULL);
        assert(strcmp(command->name, expected[index]) == 0);
    }

    static const char *const removed[] = {
        "identity",
        "config-status",
        "status",
    };
    for (size_t index = 0U; index < sizeof(removed) / sizeof(removed[0]); ++index) {
        assert(airdap_debug_shell_command_find(
            &registry,
            removed[index],
            strlen(removed[index])) == NULL);
    }
}

static void test_network_info_reports_non_secret_runtime_state(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_service_diagnostic_commands(&registry));

    captured_output_t output = {0};
    assert(run_command(&registry, "network-info", "", &output) == 0);
    assert(strcmp(
        output.text,
        "wifi=online manager_started=yes configured=yes link_connected=yes "
        "provisioning_suspended=no\n"
        "last_failure=none last_disconnect_reason=200 retry_delay_ms=0 "
        "retry_scheduled=no\n"
        "ipv4=192.168.4.20 netmask=255.255.255.0 gateway=192.168.4.1\n"
        "ap rssi_dbm=-47 channel=6\n") == 0);
    assert(strstr(output.text, "ssid") == NULL);
    assert(strstr(output.text, "bssid") == NULL);

    output = (captured_output_t) {0};
    wifi_info.ipv4_available = false;
    wifi_info.ap_available = false;
    wifi_info.last_failure = AIRDAP_WIFI_MANAGER_FAILURE_AUTHENTICATION;
    mode_snapshot.wifi = AIRDAP_WIFI_DISCONNECTED;
    assert(run_command(&registry, "network-info", "", &output) == 0);
    assert(strstr(output.text, "wifi=disconnected") != NULL);
    assert(strstr(output.text, "last_failure=authentication") != NULL);
    assert(strstr(output.text, "ipv4=unavailable\n") != NULL);
    assert(strstr(output.text, "ap=unavailable\n") != NULL);

    output = (captured_output_t) {0};
    wifi_info_result = ESP_FAIL;
    assert(run_command(&registry, "network-info", "", &output) == 1);
    assert(strcmp(
        output.text,
        "network-info: Wi-Fi status read failed: ESP_FAIL\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command(&registry, "network-info", "extra", &output) == 1);
    assert(strcmp(output.text, "usage: network-info\n") == 0);
}

static void test_dap_stats_reports_all_service_counters(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_service_diagnostic_commands(&registry));

    captured_output_t output = {0};
    assert(run_command(&registry, "dap-stats", "", &output) == 0);
    assert(strcmp(
        output.text,
        "requests accepted=101 processed=99 responses_delivered=97\n"
        "failures queue_full=2 timed_out=3 stale_requests=4 "
        "stale_responses=5 delivery=6\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command(&registry, "dap-stats", "extra", &output) == 1);
    assert(output.last_style == AIRDAP_DEBUG_SHELL_STYLE_WARNING);
    assert(strcmp(output.text, "usage: dap-stats\n") == 0);
}

static void test_system_memory_and_mode_diagnostics(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));

    captured_output_t output = {0};
    assert(run_command(&registry, "system-info", "", &output) == 0);
    assert(strcmp(
        output.text,
        "usb_serial=ADP-001122334455\n"
        "device_id=ADP-001122334455\n"
        "uuid=000102030405060708090A0B0C0D0E0F\n"
        "firmware_version=abc1234\n"
        "protocol_version=1\n"
        "capabilities=0x0000001F\n"
        "idf_version=v6.1 uptime_ms=1234567\n"
        "chip_model=esp32s3 chip_revision=2.3 chip_cores=2 "
        "chip_features=0x00000091 reset_reason=software\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command(&registry, "memory-info", "", &output) == 0);
    assert(strcmp(
        output.text,
        "default total=100001 free=100002 min_free=100003 largest=100004\n"
        "internal total=200001 free=200002 min_free=200003 largest=200004\n"
        "dma total=300001 free=300002 min_free=300003 largest=300004\n"
        "spiram total=400001 free=400002 min_free=400003 largest=400004\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command(&registry, "mode-status", "", &output) == 0);
    assert(strcmp(
        output.text,
        "schema_version=1\n"
        "provisioning_state=provisioned\n"
        "usb=present wifi=online provisioning=active ota=receiving "
        "dap_owner=usb\n") == 0);
}

static void test_ota_and_target_diagnostics(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));

    captured_output_t output = {0};
    assert(run_command(&registry, "ota-status", "", &output) == 0);
    assert(strcmp(
        output.text,
        "running_version=abc1234 protocol_version=1 max_image_size=4128768 "
        "rollback=supported session_state=receiving\n"
        "running_partition=ota_0 address=0x00010000 size=4128768 "
        "image_state=valid\n"
        "boot_partition=ota_1 address=0x00400000 size=4128768\n") == 0);

    output = (captured_output_t) {0};
    assert(run_command(&registry, "target-status", "", &output) == 0);
    assert(strcmp(
        output.text,
        "power_active=yes target_mv=3301 usb_vbus_mv=4998 dap_owner=usb\n") == 0);
}

static void test_mode_status_reports_config_read_failure(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));

    config_status_result = ESP_FAIL;
    captured_output_t output = {0};
    assert(run_command(&registry, "mode-status", "", &output) == 1);
    assert(output.last_style == AIRDAP_DEBUG_SHELL_STYLE_ERROR);
    assert(strcmp(
        output.text,
        "mode-status: config status read failed: ESP_FAIL\n") == 0);
}

static void test_tasks_reports_runtime_stack_and_affinity(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));

    captured_output_t output = {0};
    assert(run_command(&registry, "tasks", "", &output) == 0);
    assert(strcmp(
        output.text,
        "tasks=3 total_runtime_us=1000000 cpu_capacity_cores=2\n"
        "number     name             state     affinity priority   "
        "base_priority stack_free_bytes runtime_us           cpu_pct\n"
        "1          IDLE0            ready     0        0          "
        "0             512              1200000              60.00\n"
        "8          wifi             blocked   0        5          "
        "4             768              500000               25.00\n"
        "12         task-name-12345  running   any      4          "
        "4             384              100000               5.00\n") == 0);
    assert(atomic_load(&maximum_suspended_scheduler_count) == 2U);
    wait_for_schedulers_to_resume();
    assert(atomic_load(&suspended_scheduler_count) == 0U);
}

static void test_tasks_reports_interval_runtime_deltas(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));
    advance_task_runtime_on_delay = true;

    captured_output_t output = {0};
    assert(run_command(&registry, "tasks", "--interval 1000", &output) == 0);
    assert(last_delay_ticks == 1000U);
    assert(strcmp(
        output.text,
        "tasks=3 sample_ms=1000 total_delta_runtime_us=100000 "
        "cpu_capacity_cores=2\n"
        "number     name             state     affinity priority   "
        "base_priority stack_free_bytes runtime_delta_us     cpu_pct\n"
        "1          IDLE0            ready     0        0          "
        "0             512              100000               50.00\n"
        "12         task-name-12345  running   any      4          "
        "4             384              50000                25.00\n"
        "8          wifi             blocked   0        5          "
        "4             768              50000                25.00\n") == 0);
    assert(atomic_load(&maximum_suspended_scheduler_count) == 2U);
    wait_for_schedulers_to_resume();
    assert(atomic_load(&suspended_scheduler_count) == 0U);
}

static void test_commands_reject_arguments_with_usage(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));

    captured_output_t output = {0};
    assert(run_command(&registry, "tasks", "extra", &output) == 1);
    assert(output.last_style == AIRDAP_DEBUG_SHELL_STYLE_WARNING);
    assert(strcmp(output.text, "usage: tasks [--interval <ms>]\n") == 0);

    static const char *const invalid[] = {
        "--interval",
        "--interval 99",
        "--interval 5001",
        "--interval 1s",
        "--interval 1000 extra",
    };
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        output = (captured_output_t) {0};
        assert(run_command(&registry, "tasks", invalid[index], &output) == 1);
        assert(strcmp(
            output.text,
            "usage: tasks [--interval <ms>]\n") == 0);
    }
}

static void test_tasks_reports_guard_capacity_and_snapshot_failures(void)
{
    airdap_debug_shell_command_registry_t registry;
    airdap_debug_shell_command_registry_init(&registry);
    assert(airdap_debug_shell_register_diagnostic_commands(&registry));

    captured_output_t output = {0};
    ipc_result = ESP_FAIL;
    assert(run_command(&registry, "tasks", "", &output) == 1);
    assert(strcmp(
        output.text,
        "tasks: unable to suspend both schedulers: ESP_FAIL\n") == 0);
    assert(atomic_load(&suspended_scheduler_count) == 0U);

    output = (captured_output_t) {0};
    ipc_result = ESP_OK;
    reported_task_count = 49U;
    assert(run_command(&registry, "tasks", "", &output) == 1);
    assert(strcmp(
        output.text,
        "tasks: task count 49 exceeds diagnostic limit 48\n") == 0);
    wait_for_schedulers_to_resume();
    assert(atomic_load(&suspended_scheduler_count) == 0U);

    output = (captured_output_t) {0};
    reported_task_count = 3U;
    captured_task_count = 0U;
    assert(run_command(&registry, "tasks", "", &output) == 1);
    assert(strcmp(
        output.text,
        "tasks: unable to capture a stable task snapshot\n") == 0);
    wait_for_schedulers_to_resume();
    assert(atomic_load(&suspended_scheduler_count) == 0U);

    output = (captured_output_t) {0};
    captured_task_count = 3U;
    task_total_runtime = 0U;
    assert(run_command(&registry, "tasks", "", &output) == 0);
    assert(strstr(output.text, "total_runtime_us=0") != NULL);
    assert(strstr(output.text, "1200000              0.00\n") != NULL);
    assert(strstr(output.text, "500000               0.00\n") != NULL);
    assert(strstr(output.text, "100000               0.00\n") != NULL);
    wait_for_schedulers_to_resume();
    assert(atomic_load(&suspended_scheduler_count) == 0U);
}

int main(void)
{
    set_up();
    test_registers_diagnostics_with_detailed_metadata();
    set_up();
    test_registers_complete_shell_without_legacy_duplicates();
    set_up();
    test_dap_stats_reports_all_service_counters();
    set_up();
    test_network_info_reports_non_secret_runtime_state();
    set_up();
    test_system_memory_and_mode_diagnostics();
    set_up();
    test_ota_and_target_diagnostics();
    set_up();
    test_mode_status_reports_config_read_failure();
    set_up();
    test_tasks_reports_runtime_stack_and_affinity();
    set_up();
    test_tasks_reports_interval_runtime_deltas();
    set_up();
    test_commands_reject_arguments_with_usage();
    set_up();
    test_tasks_reports_guard_capacity_and_snapshot_failures();

    puts("Debug shell diagnostic command tests passed");
    return 0;
}
