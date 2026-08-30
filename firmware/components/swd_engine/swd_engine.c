#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_board_pins.h"
#include "airdap_swd.h"
#include "airdap_swd_protocol.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"

enum {
    AIRDAP_SWD_MIN_CLOCK_HZ = 100000,
    AIRDAP_SWD_MAX_CLOCK_HZ = 10000000,
    AIRDAP_SWD_DEFAULT_WAIT_RETRIES = 100,
    AIRDAP_SWD_DEFAULT_IDLE_CYCLES = 8,
    AIRDAP_SWD_DEFAULT_TURNAROUND_CYCLES = 1,
    AIRDAP_SWD_BUFFER_BYTES = 8,
    AIRDAP_DP_RDBUFF_ADDRESS = 0xC,
    AIRDAP_DP_ABORT_ADDRESS = 0x0,
    AIRDAP_DP_ABORT_CLEAR_FLAGS = 0x1E,
    AIRDAP_SWJ_PIN_SWCLK = 1U << 0,
    AIRDAP_SWJ_PIN_SWDIO = 1U << 1,
};

typedef struct {
    spi_device_handle_t spi;
    unsigned int wait_retries;
    uint8_t idle_cycles;
    uint8_t turnaround_cycles;
    bool data_phase;
    uint32_t clock_hz;
    bool initialized;
    bool host_output;
    uint8_t manual_pin_values;
    uint8_t tx_buffer[AIRDAP_SWD_BUFFER_BYTES] __attribute__((aligned(4)));
    uint8_t rx_buffer[AIRDAP_SWD_BUFFER_BYTES] __attribute__((aligned(4)));
} swd_engine_t;

static swd_engine_t engine = {
    .wait_retries = AIRDAP_SWD_DEFAULT_WAIT_RETRIES,
    .idle_cycles = AIRDAP_SWD_DEFAULT_IDLE_CYCLES,
    .turnaround_cycles = AIRDAP_SWD_DEFAULT_TURNAROUND_CYCLES,
    .manual_pin_values = AIRDAP_SWJ_PIN_SWDIO,
};

static bool clock_is_valid(uint32_t clock_hz)
{
    return clock_hz >= AIRDAP_SWD_MIN_CLOCK_HZ &&
        clock_hz <= AIRDAP_SWD_MAX_CLOCK_HZ;
}

static esp_err_t set_host_output(void *context, bool enabled)
{
    swd_engine_t *swd = context;
    esp_err_t error = gpio_set_level(
        (gpio_num_t) AIRDAP_PIN_SWDIO_DIR,
        enabled ? 1U : 0U);

    if (error == ESP_OK) {
        swd->host_output = enabled;
    }
    return error;
}

static esp_err_t write_bits(void *context, uint64_t bits, size_t bit_count)
{
    swd_engine_t *swd = context;

    if (!swd->host_output || bit_count == 0U || bit_count > 64U) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(swd->tx_buffer, 0, sizeof(swd->tx_buffer));
    memcpy(swd->tx_buffer, &bits, (bit_count + 7U) / 8U);

    spi_transaction_t transaction = {
        .length = bit_count,
        .tx_buffer = swd->tx_buffer,
    };
    return spi_device_polling_transmit(swd->spi, &transaction);
}

static esp_err_t read_bits(void *context, size_t bit_count, uint64_t *bits)
{
    swd_engine_t *swd = context;

    if (swd->host_output || bits == NULL || bit_count == 0U || bit_count > 64U) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(swd->rx_buffer, 0, sizeof(swd->rx_buffer));
    spi_transaction_t transaction = {
        .rxlength = bit_count,
        .rx_buffer = swd->rx_buffer,
    };
    esp_err_t error = spi_device_polling_transmit(swd->spi, &transaction);
    if (error != ESP_OK) {
        return error;
    }

    *bits = 0U;
    memcpy(bits, swd->rx_buffer, (bit_count + 7U) / 8U);
    if (bit_count < 64U) {
        *bits &= (UINT64_C(1) << bit_count) - 1U;
    }
    return ESP_OK;
}

static esp_err_t turnaround(void *context, bool host_output)
{
    swd_engine_t *swd = context;
    uint64_t ignored = 0U;
    esp_err_t error;

    if (host_output) {
        if (swd->host_output) {
            return ESP_ERR_INVALID_STATE;
        }

        error = read_bits(swd, swd->turnaround_cycles, &ignored);
        if (error != ESP_OK) {
            return error;
        }
        return set_host_output(swd, true);
    }

    if (!swd->host_output) {
        return ESP_ERR_INVALID_STATE;
    }
    error = set_host_output(swd, false);
    if (error != ESP_OK) {
        return error;
    }

    /*
     * The target drives ACK on the rising edge that ends host-to-target
     * turnaround. SPI mode 0 samples that edge, so the first ACK read also
     * supplies the final turnaround clock. Only preceding cycles are ignored.
     */
    if (swd->turnaround_cycles == 1U) {
        return ESP_OK;
    }
    return read_bits(swd, swd->turnaround_cycles - 1U, &ignored);
}

static airdap_swd_io_t protocol_io(void)
{
    return (airdap_swd_io_t) {
        .context = &engine,
        .idle_cycles = engine.idle_cycles,
        .data_phase = engine.data_phase,
        .set_host_output = set_host_output,
        .write_bits = write_bits,
        .read_bits = read_bits,
        .turnaround = turnaround,
    };
}

static esp_err_t add_spi_device(uint32_t clock_hz, spi_device_handle_t *device)
{
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = (int) clock_hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX |
            SPI_DEVICE_3WIRE |
            SPI_DEVICE_NO_DUMMY |
            SPI_DEVICE_TXBIT_LSBFIRST |
            SPI_DEVICE_RXBIT_LSBFIRST,
    };

    return spi_bus_add_device(SPI2_HOST, &device_config, device);
}

static esp_err_t initialize_spi_bus(uint32_t clock_hz)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = AIRDAP_PIN_TARGET_SWDIO_TMS,
        .miso_io_num = -1,
        .sclk_io_num = AIRDAP_PIN_TARGET_SWCLK_TCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AIRDAP_SWD_BUFFER_BYTES,
        /* SWDIO must return high between transactions without another clock. */
        .data_io_default_level = 1,
    };
    esp_err_t error = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_DISABLED);
    if (error != ESP_OK) {
        return error;
    }
    error = add_spi_device(clock_hz, &engine.spi);
    if (error != ESP_OK) {
        (void) spi_bus_free(SPI2_HOST);
    }
    return error;
}

esp_err_t airdap_swd_init(uint32_t clock_hz)
{
    if (!clock_is_valid(clock_hz)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (engine.initialized) {
        return airdap_swd_set_clock(clock_hz);
    }

    esp_err_t error = set_host_output(&engine, false);
    if (error != ESP_OK) {
        return error;
    }

    error = initialize_spi_bus(clock_hz);
    if (error != ESP_OK) {
        return error;
    }

    engine.clock_hz = clock_hz;
    engine.initialized = true;
    return ESP_OK;
}

esp_err_t airdap_swd_set_clock(uint32_t clock_hz)
{
    if (!clock_is_valid(clock_hz)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (engine.clock_hz == clock_hz) {
        return ESP_OK;
    }

    esp_err_t error = set_host_output(&engine, false);
    if (error != ESP_OK) {
        return error;
    }

    const uint32_t previous_clock_hz = engine.clock_hz;
    error = spi_bus_remove_device(engine.spi);
    if (error != ESP_OK) {
        return error;
    }
    engine.spi = NULL;

    error = add_spi_device(clock_hz, &engine.spi);
    if (error != ESP_OK) {
        const esp_err_t restore_error = add_spi_device(previous_clock_hz, &engine.spi);
        if (restore_error != ESP_OK) {
            (void) spi_bus_free(SPI2_HOST);
            engine.initialized = false;
        }
        return error;
    }

    engine.clock_hz = clock_hz;
    return ESP_OK;
}

void airdap_swd_set_wait_retries(unsigned int wait_retries)
{
    engine.wait_retries = wait_retries;
}

esp_err_t airdap_swd_configure_transfer(
    uint8_t idle_cycles,
    unsigned int wait_retries)
{
    engine.idle_cycles = idle_cycles;
    engine.wait_retries = wait_retries;
    return ESP_OK;
}

esp_err_t airdap_swd_configure_bus(
    uint8_t turnaround_cycles,
    bool data_phase)
{
    if (turnaround_cycles < 1U || turnaround_cycles > 4U) {
        return ESP_ERR_INVALID_ARG;
    }
    engine.turnaround_cycles = turnaround_cycles;
    engine.data_phase = data_phase;
    return ESP_OK;
}

esp_err_t airdap_swd_set_io_state(bool host_output)
{
    if (!engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (engine.host_output == host_output) {
        return ESP_OK;
    }
    return set_host_output(&engine, host_output);
}

esp_err_t airdap_swd_write_sequence(
    const uint8_t *data,
    size_t bit_count)
{
    if (!engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || bit_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = airdap_swd_set_io_state(true);
    if (error != ESP_OK) {
        return error;
    }

    size_t bit_offset = 0U;
    while (bit_offset < bit_count) {
        const size_t chunk_bits = (bit_count - bit_offset) > 64U
            ? 64U
            : bit_count - bit_offset;
        uint64_t bits = 0U;
        memcpy(&bits, data + (bit_offset / 8U), (chunk_bits + 7U) / 8U);
        error = write_bits(&engine, bits, chunk_bits);
        if (error != ESP_OK) {
            return error;
        }
        bit_offset += chunk_bits;
    }
    return ESP_OK;
}

esp_err_t airdap_swd_read_sequence(
    uint8_t *data,
    size_t bit_count)
{
    if (!engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || bit_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = airdap_swd_set_io_state(false);
    if (error != ESP_OK) {
        return error;
    }

    memset(data, 0, (bit_count + 7U) / 8U);
    size_t bit_offset = 0U;
    while (bit_offset < bit_count) {
        const size_t chunk_bits = (bit_count - bit_offset) > 64U
            ? 64U
            : bit_count - bit_offset;
        uint64_t bits = 0U;
        error = read_bits(&engine, chunk_bits, &bits);
        if (error != ESP_OK) {
            return error;
        }
        memcpy(data + (bit_offset / 8U), &bits, (chunk_bits + 7U) / 8U);
        bit_offset += chunk_bits;
    }
    return ESP_OK;
}

esp_err_t airdap_swd_drive_pins(
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins)
{
    const uint8_t supported = AIRDAP_SWJ_PIN_SWCLK | AIRDAP_SWJ_PIN_SWDIO;
    if (!engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pins == NULL || (select & (uint8_t) ~supported) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    engine.manual_pin_values =
        (engine.manual_pin_values & (uint8_t) ~select) |
        (value & select);

    esp_err_t error = set_host_output(&engine, false);
    if (error != ESP_OK) {
        return error;
    }
    error = spi_bus_remove_device(engine.spi);
    if (error != ESP_OK) {
        return error;
    }
    engine.spi = NULL;
    error = spi_bus_free(SPI2_HOST);
    if (error != ESP_OK) {
        return error;
    }

    (void) gpio_set_level(
        (gpio_num_t) AIRDAP_PIN_TARGET_SWCLK_TCK,
        (engine.manual_pin_values & AIRDAP_SWJ_PIN_SWCLK) != 0U);
    (void) gpio_set_level(
        (gpio_num_t) AIRDAP_PIN_TARGET_SWDIO_TMS,
        (engine.manual_pin_values & AIRDAP_SWJ_PIN_SWDIO) != 0U);

    const gpio_config_t pin_config = {
        .pin_bit_mask =
            (UINT64_C(1) << AIRDAP_PIN_TARGET_SWCLK_TCK) |
            (UINT64_C(1) << AIRDAP_PIN_TARGET_SWDIO_TMS),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&pin_config);
    if (error == ESP_OK) {
        error = set_host_output(&engine, true);
    }
    if (error == ESP_OK && wait_us > 0U) {
        esp_rom_delay_us(wait_us > 3000000U ? 3000000U : wait_us);
    }

    if (error == ESP_OK) {
        *pins = 0U;
        if (gpio_get_level((gpio_num_t) AIRDAP_PIN_TARGET_SWCLK_TCK) != 0) {
            *pins |= AIRDAP_SWJ_PIN_SWCLK;
        }
        if (gpio_get_level((gpio_num_t) AIRDAP_PIN_TARGET_SWDIO_TMS) != 0) {
            *pins |= AIRDAP_SWJ_PIN_SWDIO;
        }
    }

    (void) set_host_output(&engine, false);
    (void) gpio_reset_pin((gpio_num_t) AIRDAP_PIN_TARGET_SWCLK_TCK);
    (void) gpio_reset_pin((gpio_num_t) AIRDAP_PIN_TARGET_SWDIO_TMS);
    const esp_err_t restore_error = initialize_spi_bus(engine.clock_hz);
    if (restore_error != ESP_OK) {
        engine.initialized = false;
        return error == ESP_OK ? restore_error : error;
    }
    const esp_err_t direction_error = set_host_output(&engine, true);
    if (error != ESP_OK) {
        return error;
    }
    return direction_error;
}

esp_err_t airdap_swd_connect(uint32_t *idcode)
{
    if (!engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const airdap_swd_io_t io = protocol_io();
    return airdap_swd_protocol_connect(&io, idcode);
}

esp_err_t airdap_swd_transfer(
    const airdap_swd_request_t *request,
    uint32_t *data,
    airdap_swd_ack_t *ack)
{
    if (!engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!engine.host_output) {
        esp_err_t error = set_host_output(&engine, true);
        if (error != ESP_OK) {
            return error;
        }
    }

    const airdap_swd_io_t io = protocol_io();
    return airdap_swd_protocol_transfer(
        &io,
        request,
        data,
        engine.wait_retries,
        ack);
}

esp_err_t airdap_swd_read_dp(
    uint8_t address,
    uint32_t *data,
    airdap_swd_ack_t *ack)
{
    return airdap_swd_transfer(
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_READ,
            .address = address,
        },
        data,
        ack);
}

esp_err_t airdap_swd_write_dp(
    uint8_t address,
    uint32_t data,
    airdap_swd_ack_t *ack)
{
    return airdap_swd_transfer(
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_WRITE,
            .address = address,
        },
        &data,
        ack);
}

esp_err_t airdap_swd_read_ap(
    uint8_t address,
    uint32_t *data,
    airdap_swd_ack_t *ack)
{
    uint32_t posted_data = 0U;
    esp_err_t error = airdap_swd_transfer(
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_AP,
            .direction = AIRDAP_SWD_READ,
            .address = address,
        },
        &posted_data,
        ack);
    if (error != ESP_OK || *ack != AIRDAP_SWD_ACK_OK) {
        return error;
    }

    return airdap_swd_read_dp(AIRDAP_DP_RDBUFF_ADDRESS, data, ack);
}

esp_err_t airdap_swd_write_ap(
    uint8_t address,
    uint32_t data,
    airdap_swd_ack_t *ack)
{
    return airdap_swd_transfer(
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_AP,
            .direction = AIRDAP_SWD_WRITE,
            .address = address,
        },
        &data,
        ack);
}

esp_err_t airdap_swd_clear_errors(airdap_swd_ack_t *ack)
{
    return airdap_swd_write_dp(
        AIRDAP_DP_ABORT_ADDRESS,
        AIRDAP_DP_ABORT_CLEAR_FLAGS,
        ack);
}
