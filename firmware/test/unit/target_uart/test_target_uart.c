#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airdap_board_pins.h"
#include "airdap_target_uart.h"
#include "airdap_target_uart_internal.h"
#include "driver/uart.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    TARGET_UART_RX_BUFFER_SIZE = 2048,
    TARGET_UART_TX_BUFFER_SIZE = 2048,
    TARGET_UART_WORKER_STACK_SIZE = 3072,
    TARGET_UART_WORKER_PRIORITY = 5,
};

struct fake_semaphore {
    pthread_mutex_t mutex;
};

static esp_err_t param_result = ESP_OK;
static esp_err_t pin_result = ESP_OK;
static esp_err_t install_result = ESP_OK;
static BaseType_t task_create_result = pdPASS;
static unsigned int param_calls;
static unsigned int pin_calls;
static unsigned int install_calls;
static unsigned int delete_calls;
static unsigned int task_create_calls;
static uart_port_t configured_port;
static uart_config_t configured;
static int configured_tx;
static int configured_rx;
static int configured_rts;
static int configured_cts;
static int installed_rx_buffer;
static int installed_tx_buffer;
static int installed_queue_size;
static QueueHandle_t *installed_queue;
static int installed_flags;
static uint8_t read_payload[] = {0x12, 0x34, 0x56};
static const void *last_write_data;
static size_t last_write_size;
static TickType_t last_read_timeout;
static int read_result = 3;
static int write_result = 2;
static esp_err_t rx_buffer_result = ESP_OK;
static esp_err_t tx_buffer_result = ESP_OK;
static size_t rx_buffered_bytes = 17U;
static size_t tx_buffer_free_bytes = 1900U;
static TaskFunction_t worker_task;
static bool expect_initialized_before_task_start;

static void reset_driver_fakes(void)
{
    param_result = ESP_OK;
    pin_result = ESP_OK;
    install_result = ESP_OK;
    task_create_result = pdPASS;
    param_calls = 0U;
    pin_calls = 0U;
    install_calls = 0U;
    delete_calls = 0U;
    task_create_calls = 0U;
    memset(&configured, 0, sizeof(configured));
    last_write_data = NULL;
    last_write_size = 0U;
    last_read_timeout = 0U;
    read_result = 3;
    write_result = 2;
    rx_buffer_result = ESP_OK;
    tx_buffer_result = ESP_OK;
    worker_task = NULL;
    expect_initialized_before_task_start = false;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_semaphore *semaphore = calloc(1U, sizeof(*semaphore));
    assert(semaphore != NULL);
    assert(pthread_mutex_init(&semaphore->mutex, NULL) == 0);
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore != NULL && timeout == portMAX_DELAY);
    return pthread_mutex_lock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL);
    return pthread_mutex_unlock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL);
    assert(pthread_mutex_destroy(&semaphore->mutex) == 0);
    free(semaphore);
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core_id)
{
    assert(task != NULL);
    assert(strcmp(name, "target_uart") == 0);
    assert(stack_depth == TARGET_UART_WORKER_STACK_SIZE);
    assert(argument == NULL);
    assert(priority == TARGET_UART_WORKER_PRIORITY);
    assert(handle == NULL);
    assert(core_id == 1);
    ++task_create_calls;
    worker_task = task;
    if (expect_initialized_before_task_start) {
        airdap_target_uart_status_t status;
        assert(airdap_target_uart_get_status(&status) == ESP_OK);
        assert(status.initialized);
    }
    return task_create_result;
}

esp_err_t uart_param_config(
    uart_port_t uart_num,
    const uart_config_t *uart_config)
{
    assert(uart_config != NULL);
    ++param_calls;
    configured_port = uart_num;
    configured = *uart_config;
    return param_result;
}

esp_err_t uart_set_pin(
    uart_port_t uart_num,
    int tx_io_num,
    int rx_io_num,
    int rts_io_num,
    int cts_io_num)
{
    ++pin_calls;
    configured_port = uart_num;
    configured_tx = tx_io_num;
    configured_rx = rx_io_num;
    configured_rts = rts_io_num;
    configured_cts = cts_io_num;
    return pin_result;
}

esp_err_t uart_driver_install(
    uart_port_t uart_num,
    int rx_buffer_size,
    int tx_buffer_size,
    int queue_size,
    QueueHandle_t *uart_queue,
    int intr_alloc_flags)
{
    ++install_calls;
    configured_port = uart_num;
    installed_rx_buffer = rx_buffer_size;
    installed_tx_buffer = tx_buffer_size;
    installed_queue_size = queue_size;
    installed_queue = uart_queue;
    installed_flags = intr_alloc_flags;
    return install_result;
}

esp_err_t uart_driver_delete(uart_port_t uart_num)
{
    assert(uart_num == UART_NUM_1);
    ++delete_calls;
    return ESP_OK;
}

int uart_read_bytes(
    uart_port_t uart_num,
    void *buffer,
    uint32_t length,
    TickType_t ticks_to_wait)
{
    assert(uart_num == UART_NUM_1);
    assert(buffer != NULL);
    last_read_timeout = ticks_to_wait;
    if (read_result <= 0) {
        return read_result;
    }
    const size_t available = (size_t) read_result < sizeof(read_payload)
        ? (size_t) read_result
        : sizeof(read_payload);
    const size_t count = length < available ? length : available;
    memcpy(buffer, read_payload, count);
    return (int) count;
}

int uart_write_bytes(uart_port_t uart_num, const void *source, size_t size)
{
    assert(uart_num == UART_NUM_1);
    last_write_data = source;
    last_write_size = size;
    return write_result;
}

esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size)
{
    assert(uart_num == UART_NUM_1);
    assert(size != NULL);
    if (rx_buffer_result == ESP_OK) {
        *size = rx_buffered_bytes;
    }
    return rx_buffer_result;
}

esp_err_t uart_get_tx_buffer_free_size(uart_port_t uart_num, size_t *size)
{
    assert(uart_num == UART_NUM_1);
    assert(size != NULL);
    if (tx_buffer_result == ESP_OK) {
        *size = tx_buffer_free_bytes;
    }
    return tx_buffer_result;
}

static void assert_config(
    int baud,
    uart_word_length_t data_bits,
    uart_parity_t parity,
    uart_stop_bits_t stop_bits)
{
    assert(configured_port == UART_NUM_1);
    assert(configured.baud_rate == baud);
    assert(configured.data_bits == data_bits);
    assert(configured.parity == parity);
    assert(configured.stop_bits == stop_bits);
    assert(configured.flow_ctrl == UART_HW_FLOWCTRL_DISABLE);
    assert(configured.source_clk == UART_SCLK_DEFAULT);
}

static void test_service_rejected_before_initialization(void)
{
    airdap_target_uart_session_id_t session = 0U;
    airdap_target_uart_status_t status;

    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        false,
        &session) == AIRDAP_TARGET_UART_INVALID_STATE);
    assert(airdap_target_uart_get_status(NULL) == ESP_ERR_INVALID_ARG);
    assert(airdap_target_uart_get_status(&status) == ESP_OK);
    assert(!status.initialized);
    assert(!airdap_target_uart_process_rx_once());
}

static void test_initialization_retry_and_configuration(void)
{
    reset_driver_fakes();

    param_result = ESP_FAIL;
    assert(airdap_target_uart_init() == ESP_FAIL);
    assert(param_calls == 1U && pin_calls == 0U && install_calls == 0U);

    param_result = ESP_OK;
    pin_result = ESP_FAIL;
    assert(airdap_target_uart_init() == ESP_FAIL);
    assert(param_calls == 2U && pin_calls == 1U && install_calls == 0U);

    pin_result = ESP_OK;
    install_result = ESP_FAIL;
    assert(airdap_target_uart_init() == ESP_FAIL);
    assert(param_calls == 3U && pin_calls == 2U && install_calls == 1U);

    install_result = ESP_OK;
    task_create_result = pdFAIL;
    assert(airdap_target_uart_init() == ESP_ERR_NO_MEM);
    assert(delete_calls == 1U);
    airdap_target_uart_status_t status;
    assert(airdap_target_uart_get_status(&status) == ESP_OK);
    assert(!status.initialized);

    task_create_result = pdPASS;
    expect_initialized_before_task_start = true;
    assert(airdap_target_uart_init() == ESP_OK);
    assert(task_create_calls == 2U && worker_task != NULL);
    assert_config(
        AIRDAP_TARGET_UART_DEFAULT_BAUD,
        UART_DATA_8_BITS,
        UART_PARITY_DISABLE,
        UART_STOP_BITS_1);
    assert(configured_tx == AIRDAP_PIN_TARGET_TX_TDI);
    assert(configured_rx == AIRDAP_PIN_TARGET_RX_TDO);
    assert(configured_rts == UART_PIN_NO_CHANGE);
    assert(configured_cts == UART_PIN_NO_CHANGE);
    assert(installed_rx_buffer == TARGET_UART_RX_BUFFER_SIZE);
    assert(installed_tx_buffer == TARGET_UART_TX_BUFFER_SIZE);
    assert(installed_queue_size == 0);
    assert(installed_queue == NULL);
    assert(installed_flags == 0);

    assert(airdap_target_uart_init() == ESP_OK);
    assert(task_create_calls == 2U);
}

static void open_pair(
    airdap_target_uart_session_id_t *usb_session,
    airdap_target_uart_session_id_t *network_session)
{
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        false,
        usb_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        true,
        network_session) == AIRDAP_TARGET_UART_OK);
}

static void close_pair(
    airdap_target_uart_session_id_t usb_session,
    airdap_target_uart_session_id_t network_session)
{
    assert(airdap_target_uart_session_close(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_session_close(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session) == AIRDAP_TARGET_UART_OK);
}

static void test_admission_and_independent_fanout(void)
{
    airdap_target_uart_session_id_t usb_session = 0U;
    airdap_target_uart_session_id_t network_session = 0U;
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_COUNT,
        false,
        &usb_session) == AIRDAP_TARGET_UART_INVALID_ARGUMENT);
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        false,
        NULL) == AIRDAP_TARGET_UART_INVALID_ARGUMENT);
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        false,
        &network_session) == AIRDAP_TARGET_UART_UNAUTHENTICATED);

    open_pair(&usb_session, &network_session);
    airdap_target_uart_session_id_t duplicate = 0U;
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        false,
        &duplicate) == AIRDAP_TARGET_UART_BUSY);

    static const uint8_t input[] = {1U, 2U, 3U};
    airdap_target_uart_publish_rx(input, sizeof(input));

    uint8_t output[4] = {0};
    size_t received = 99U;
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        output,
        2U,
        &received) == AIRDAP_TARGET_UART_OK);
    assert(received == 2U && memcmp(output, input, 2U) == 0);
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session,
        output,
        sizeof(output),
        &received) == AIRDAP_TARGET_UART_OK);
    assert(received == sizeof(input) &&
        memcmp(output, input, sizeof(input)) == 0);
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        output,
        sizeof(output),
        &received) == AIRDAP_TARGET_UART_OK);
    assert(received == 1U && output[0] == 3U);

    close_pair(usb_session, network_session);
}

static void test_slow_subscriber_drops_only_its_own_bytes(void)
{
    airdap_target_uart_session_id_t usb_session = 0U;
    airdap_target_uart_session_id_t network_session = 0U;
    open_pair(&usb_session, &network_session);

    uint8_t fill[AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE];
    for (size_t index = 0U; index < sizeof(fill); ++index) {
        fill[index] = (uint8_t) index;
    }
    airdap_target_uart_publish_rx(fill, sizeof(fill));

    uint8_t output[AIRDAP_TARGET_UART_SUBSCRIBER_BUFFER_SIZE];
    size_t received = 0U;
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        output,
        sizeof(output),
        &received) == AIRDAP_TARGET_UART_OK);
    assert(received == sizeof(fill) && memcmp(output, fill, sizeof(fill)) == 0);

    static const uint8_t tail[] = {0xA1U, 0xA2U, 0xA3U};
    airdap_target_uart_publish_rx(tail, sizeof(tail));
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        output,
        sizeof(output),
        &received) == AIRDAP_TARGET_UART_OK);
    assert(received == sizeof(tail) &&
        memcmp(output, tail, sizeof(tail)) == 0);

    airdap_target_uart_session_status_t usb_status;
    airdap_target_uart_session_status_t network_status;
    assert(airdap_target_uart_session_get_status(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        &usb_status) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_session_get_status(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session,
        &network_status) == AIRDAP_TARGET_UART_OK);
    assert(usb_status.rx_buffered_bytes == 0U);
    assert(usb_status.rx_dropped_bytes == 0U);
    assert(network_status.rx_buffered_bytes == sizeof(fill));
    assert(network_status.rx_dropped_bytes == sizeof(tail));

    close_pair(usb_session, network_session);
}

static void test_tx_owner_configuration_and_io(void)
{
    airdap_target_uart_session_id_t usb_session = 0U;
    airdap_target_uart_session_id_t network_session = 0U;
    open_pair(&usb_session, &network_session);

    assert(airdap_target_uart_tx_acquire(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_tx_acquire(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session) == AIRDAP_TARGET_UART_ALREADY_OWNER);
    assert(airdap_target_uart_tx_acquire(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session) == AIRDAP_TARGET_UART_BUSY);

    const unsigned int params_before = param_calls;
    assert(airdap_target_uart_session_configure(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session,
        9600U,
        0U,
        0U,
        8U) == AIRDAP_TARGET_UART_NOT_OWNER);
    assert(param_calls == params_before);

    assert(airdap_target_uart_session_configure(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        1000000U,
        2U,
        2U,
        7U) == AIRDAP_TARGET_UART_OK);
    assert_config(1000000, UART_DATA_7_BITS, UART_PARITY_EVEN, UART_STOP_BITS_2);
    assert(airdap_target_uart_session_configure(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        5000001U,
        0U,
        0U,
        8U) == AIRDAP_TARGET_UART_INVALID_ARGUMENT);

    param_result = ESP_FAIL;
    assert(airdap_target_uart_session_configure(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        115200U,
        0U,
        0U,
        8U) == AIRDAP_TARGET_UART_IO_ERROR);
    param_result = ESP_OK;

    static const uint8_t outbound[] = {0xAAU, 0x55U, 0x11U};
    size_t written = 99U;
    assert(airdap_target_uart_session_write(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session,
        outbound,
        sizeof(outbound),
        &written) == AIRDAP_TARGET_UART_NOT_OWNER);
    assert(written == 0U);
    write_result = 2;
    assert(airdap_target_uart_session_write(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        outbound,
        sizeof(outbound),
        &written) == AIRDAP_TARGET_UART_OK);
    assert(written == 2U);
    assert(last_write_data == outbound && last_write_size == sizeof(outbound));
    write_result = -1;
    assert(airdap_target_uart_session_write(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        outbound,
        sizeof(outbound),
        &written) == AIRDAP_TARGET_UART_IO_ERROR);
    assert(written == 0U);
    write_result = 2;

    assert(airdap_target_uart_session_close(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_tx_acquire(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_session_close(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session) == AIRDAP_TARGET_UART_OK);
}

static void test_stale_session_cannot_affect_replacement_owner(void)
{
    airdap_target_uart_session_id_t old_session = 0U;
    airdap_target_uart_session_id_t new_session = 0U;
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        false,
        &old_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_tx_acquire(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        old_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_session_close(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        old_session) == AIRDAP_TARGET_UART_OK);
    assert(airdap_target_uart_session_open(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        false,
        &new_session) == AIRDAP_TARGET_UART_OK);
    assert(new_session != old_session);
    assert(airdap_target_uart_tx_acquire(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        new_session) == AIRDAP_TARGET_UART_OK);

    uint8_t data = 0U;
    size_t count = 99U;
    assert(airdap_target_uart_session_close(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        old_session) == AIRDAP_TARGET_UART_STALE_SESSION);
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        old_session,
        &data,
        1U,
        &count) == AIRDAP_TARGET_UART_STALE_SESSION);
    assert(count == 0U);
    assert(airdap_target_uart_session_write(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        old_session,
        &data,
        1U,
        &count) == AIRDAP_TARGET_UART_STALE_SESSION);
    assert(count == 0U);

    airdap_target_uart_session_status_t status;
    assert(airdap_target_uart_session_get_status(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        new_session,
        &status) == AIRDAP_TARGET_UART_OK);
    assert(status.tx_owner);
    assert(airdap_target_uart_session_close(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        new_session) == AIRDAP_TARGET_UART_OK);
}

static void test_worker_and_driver_diagnostics(void)
{
    airdap_target_uart_session_id_t usb_session = 0U;
    airdap_target_uart_session_id_t network_session = 0U;
    open_pair(&usb_session, &network_session);

    read_result = 3;
    assert(airdap_target_uart_process_rx_once());
    assert(last_read_timeout == pdMS_TO_TICKS(20));
    uint8_t output[4] = {0};
    size_t received = 0U;
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_USB,
        usb_session,
        output,
        sizeof(output),
        &received) == AIRDAP_TARGET_UART_OK);
    assert(received == sizeof(read_payload));
    assert(memcmp(output, read_payload, sizeof(read_payload)) == 0);
    assert(airdap_target_uart_session_read(
        AIRDAP_TARGET_UART_TRANSPORT_NETWORK,
        network_session,
        output,
        sizeof(output),
        &received) == AIRDAP_TARGET_UART_OK);
    assert(received == sizeof(read_payload));

    read_result = -1;
    assert(!airdap_target_uart_process_rx_once());

    airdap_target_uart_status_t status;
    assert(airdap_target_uart_get_status(&status) == ESP_OK);
    assert(status.initialized);
    assert(status.baud_rate == 1000000U);
    assert(status.data_bits == 7U);
    assert(status.parity == 2U);
    assert(status.stop_bits == 2U);
    assert(status.rx_buffered_bytes == 17U);
    assert(status.tx_buffer_free_bytes == 1900U);
    assert(status.rx_bytes == sizeof(read_payload));
    assert(status.tx_bytes == 2U);
    assert(status.read_failures == 1U);
    assert(status.write_failures == 1U);

    rx_buffer_result = ESP_FAIL;
    assert(airdap_target_uart_get_status(&status) == ESP_FAIL);
    rx_buffer_result = ESP_OK;
    tx_buffer_result = ESP_FAIL;
    assert(airdap_target_uart_get_status(&status) == ESP_FAIL);
    tx_buffer_result = ESP_OK;

    close_pair(usb_session, network_session);
}

int main(void)
{
    test_service_rejected_before_initialization();
    test_initialization_retry_and_configuration();
    test_admission_and_independent_fanout();
    test_slow_subscriber_drops_only_its_own_bytes();
    test_tx_owner_configuration_and_io();
    test_stale_session_cannot_affect_replacement_owner();
    test_worker_and_driver_diagnostics();

    puts("target UART service tests passed");
    return 0;
}
