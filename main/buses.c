#include "peripherals.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    spi_transaction_ext_t master;
    spi_slave_transaction_t slave;
    twai_frame_t can;
    uint8_t can_data[8];
    bool pending, slave_disabled;
    int ready;
    QueueHandle_t uart_events;
} bus_state;
static bool i2c_received(i2c_slave_dev_handle_t dev, const i2c_slave_rx_done_event_data_t *event,
                         void *ctx) {
    (void)dev;
    v3_hardware *h = ctx;
    BaseType_t wake = pdFALSE;
    for (size_t offset = 0; offset < event->length; offset += 128) {
        v3_packet p = {.id = atomic_fetch_add(&h->sequence, 1) + 1, .time = esp_timer_get_time()};
        p.length = event->length - offset < 128 ? event->length - offset : 128;
        p.flags = offset + p.length == event->length ? 1 : 0;
        memcpy(p.data, event->buffer + offset, p.length);
        if (xQueueSendFromISR(h->events, &p, &wake) != pdTRUE)
            atomic_fetch_add(&h->dropped, 1);
        else
            atomic_fetch_add(&h->received, 1);
    }
    return wake == pdTRUE;
}
static bool i2c_requested(i2c_slave_dev_handle_t dev, const i2c_slave_request_event_data_t *event,
                          void *ctx) {
    (void)dev;
    (void)event;
    v3_hardware *h = ctx;
    atomic_fetch_add(&h->errors, 1);
    return false;
}
static bool can_received(twai_node_handle_t node, const twai_rx_done_event_data_t *event,
                         void *ctx) {
    (void)event;
    v3_hardware *h = ctx;
    v3_packet p = {.time = esp_timer_get_time()};
    twai_frame_t f = {.buffer = p.data, .buffer_len = 8};
    BaseType_t wake = pdFALSE;
    if (twai_node_receive_from_isr(node, &f) == ESP_OK) {
        p.id = f.header.id;
        p.length = f.header.dlc > 8 ? 8 : f.header.dlc;
        p.flags = (f.header.ide ? 1 : 0) | (f.header.rtr ? 2 : 0);
        if (xQueueSendFromISR(h->events, &p, &wake) != pdTRUE)
            atomic_fetch_add(&h->dropped, 1);
        else
            atomic_fetch_add(&h->received, 1);
    }
    return wake == pdTRUE;
}
static bool can_sent(twai_node_handle_t node, const twai_tx_done_event_data_t *event, void *ctx) {
    (void)node;
    v3_hardware *h = ctx;
    atomic_store(&h->actual, event->is_tx_success ? 1 : 2);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(h->done, &wake);
    return wake == pdTRUE;
}
static void spi_ready(spi_slave_transaction_t *t) {
    v3_hardware *h = t->user;
    bus_state *b = h->secondary;
    if (b->ready >= 0)
        gpio_set_level(b->ready, 1);
}
static void spi_finished(spi_slave_transaction_t *t) {
    v3_hardware *h = t->user;
    bus_state *b = h->secondary;
    if (b->ready >= 0)
        gpio_set_level(b->ready, 0);
}
int v3_bus_open(v3_hardware *h, const v3_work *w) {
    (void)w;
    const cJSON *c = v3_hw_config(h), *p = v3_hw_pins(h);
    int err;
    if (h->kind == V_I2C_DEVICE || h->kind == V_SPI_DEVICE) {
        v3_hardware *parent = v3_hw_parent(h);
        if (!parent || !parent->memory)
            return ESP_ERR_INVALID_STATE;
        h->memory = parent->memory;
        h->memory_size = parent->memory_size;
        h->shared_memory = true;
    } else if (h->kind != V_TWAI) {
        h->memory_size =
            (h->kind == V_UART || h->kind == V_I2C_SLAVE) ? V3_BUFFER_MAX : V3_BUFFER_MAX * 2;
        h->memory = heap_caps_calloc(1, h->memory_size, MALLOC_CAP_DMA);
        if (!h->memory)
            return ESP_ERR_NO_MEM;
    }
    if (LV_HAS_BUS && h->kind == V_UART) {
        const char *parity = v3_str(c, "parity", "none");
        uart_config_t u = {
            .baud_rate = v3_int(c, "baud", 115200),
            .data_bits = v3_int(c, "data_bits", 8) - 5,
            .parity = !strcmp(parity, "even")  ? UART_PARITY_EVEN
                      : !strcmp(parity, "odd") ? UART_PARITY_ODD
                                               : UART_PARITY_DISABLE,
            .stop_bits = v3_int(c, "stop_bits", 1) == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
            .flow_ctrl = v3_bool(c, "flow_control", false) ? UART_HW_FLOWCTRL_CTS_RTS
                                                           : UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = v3_int(c, "rx_threshold", 64),
            .source_clk = UART_SCLK_DEFAULT};
        if ((err = uart_param_config(h->controller, &u)))
            return err;
        if ((err = uart_set_pin(h->controller, v3_int(p, "tx", -1), v3_int(p, "rx", -1),
                                v3_int(p, "rts", -1), v3_int(p, "cts", -1))))
            return err;
        bus_state *state = calloc(1, sizeof(*state));
        if (!state)
            return ESP_ERR_NO_MEM;
        h->secondary = state;
        h->events = xQueueCreate(8, sizeof(v3_packet));
        if (!h->events)
            return ESP_ERR_NO_MEM;
        // TX uses the FIFO API with bounded polling; no hidden blocking ring-buffer write.
        if ((err = uart_driver_install(h->controller, v3_int(c, "rx_buffer", 1024), 0, 16,
                                       &state->uart_events, 0)))
            return err;
        h->installed = true;
        if ((err = uart_set_line_inverse(h->controller, v3_int(c, "invert_mask", 0))))
            return err;
        if ((err = uart_set_loop_back(h->controller, v3_bool(c, "loopback", false))))
            return err;
        return uart_set_mode(h->controller, !strcmp(v3_str(c, "mode", "uart"), "rs485")
                                                ? UART_MODE_RS485_HALF_DUPLEX
                                                : UART_MODE_UART);
    }
    if (h->kind == V_I2C) {
        i2c_master_bus_config_t b = {.i2c_port = 0,
                                     .sda_io_num = v3_int(p, "sda", -1),
                                     .scl_io_num = v3_int(p, "scl", -1),
                                     .clk_source = I2C_CLK_SRC_DEFAULT,
                                     .glitch_ignore_cnt = v3_int(c, "glitch_ignore", 7),
                                     .flags.enable_internal_pullup = v3_bool(c, "pullup", false)};
        i2c_master_bus_handle_t handle = NULL;
        err = i2c_new_master_bus(&b, &handle);
        h->handle = handle;
        return err;
    }
    if (h->kind == V_I2C_DEVICE) {
        v3_hardware *parent = v3_hw_parent(h);
        if (!parent || !parent->handle)
            return ESP_ERR_INVALID_STATE;
        i2c_device_config_t d = {.dev_addr_length = v3_int(c, "address_bits", 7) == 10
                                                        ? I2C_ADDR_BIT_LEN_10
                                                        : I2C_ADDR_BIT_LEN_7,
                                 .device_address = v3_int(c, "address", 0),
                                 .scl_speed_hz = v3_int(c, "frequency", 100000),
                                 .scl_wait_us = v3_int(c, "scl_wait_us", 0)};
        i2c_master_dev_handle_t handle = NULL;
        err = i2c_master_bus_add_device(parent->handle, &d, &handle);
        h->handle = handle;
        return err;
    }
    if (LV_HAS_SLAVE && h->kind == V_I2C_SLAVE) {
        i2c_slave_config_t d = {.i2c_port = 0,
                                .sda_io_num = v3_int(p, "sda", -1),
                                .scl_io_num = v3_int(p, "scl", -1),
                                .clk_source = I2C_CLK_SRC_DEFAULT,
                                .slave_addr = v3_int(c, "address", 0),
                                .addr_bit_len = v3_int(c, "address_bits", 7) == 10
                                                    ? I2C_ADDR_BIT_LEN_10
                                                    : I2C_ADDR_BIT_LEN_7,
                                .send_buf_depth = v3_int(c, "buffer_bytes", 1024),
                                .receive_buf_depth = v3_int(c, "buffer_bytes", 1024),
                                .flags.enable_internal_pullup = v3_bool(c, "pullup", false)};
        h->events = xQueueCreate(8, sizeof(v3_packet));
        if (!h->events)
            return ESP_ERR_NO_MEM;
        i2c_slave_dev_handle_t handle = NULL;
        if ((err = i2c_new_slave_device(&d, &handle)))
            return err;
        h->handle = handle;
        i2c_slave_event_callbacks_t cb = {.on_receive = i2c_received, .on_request = i2c_requested};
        return i2c_slave_register_event_callbacks(handle, &cb, h);
    }
    if (LV_HAS_BUS && (h->kind == V_SPI || (LV_HAS_SLAVE && h->kind == V_SPI_SLAVE))) {
        spi_bus_config_t b = {.sclk_io_num = v3_int(p, "sclk", -1),
                              .mosi_io_num = v3_int(p, "mosi", -1),
                              .miso_io_num = v3_int(p, "miso", -1),
                              .quadwp_io_num = v3_int(p, "wp", -1),
                              .quadhd_io_num = v3_int(p, "hd", -1),
                              .max_transfer_sz = v3_int(c, "max_transfer", 4096),
                              .flags = v3_bool(c, "quad", false) ? SPICOMMON_BUSFLAG_QUAD : 0,
                              .data4_io_num = -1,
                              .data5_io_num = -1,
                              .data6_io_num = -1,
                              .data7_io_num = -1};
        if (h->kind == V_SPI) {
            err = spi_bus_initialize(SPI2_HOST, &b, SPI_DMA_CH_AUTO);
            if (!err)
                h->installed = true;
            return err;
        }
        bus_state *s = calloc(1, sizeof(*s));
        if (!s)
            return ESP_ERR_NO_MEM;
        h->secondary = s;
        s->ready = v3_int(p, "ready", -1);
        if (s->ready >= 0) {
            gpio_set_level(s->ready, 0);
            if ((err = gpio_set_direction(s->ready, GPIO_MODE_OUTPUT)))
                return err;
        }
        spi_slave_interface_config_t slave = {
            .spics_io_num = v3_int(p, "cs", -1),
            .mode = v3_int(c, "mode", 0),
            .queue_size = v3_int(c, "queue_depth", 1),
            .flags = v3_bool(c, "lsb_first", false) ? SPI_SLAVE_BIT_LSBFIRST : 0,
            .post_setup_cb = spi_ready,
            .post_trans_cb = spi_finished};
        err = spi_slave_initialize(SPI2_HOST, &b, &slave, SPI_DMA_CH_AUTO);
        if (!err)
            h->installed = true;
        return err;
    }
    if (LV_HAS_BUS && h->kind == V_SPI_DEVICE) {
        v3_hardware *parent = v3_hw_parent(h);
        if (!parent || !parent->installed)
            return ESP_ERR_INVALID_STATE;
        h->secondary = calloc(1, sizeof(bus_state));
        if (!h->secondary)
            return ESP_ERR_NO_MEM;
        spi_device_interface_config_t d = {
            .clock_speed_hz = v3_int(c, "frequency", 1000000),
            .mode = v3_int(c, "mode", 0),
            .spics_io_num = v3_int(p, "cs", -1),
            .command_bits = v3_int(c, "command_bits", 0),
            .address_bits = v3_int(c, "address_bits", 0),
            .dummy_bits = v3_int(c, "dummy_bits", 0),
            .queue_size = v3_int(c, "queue_depth", 1),
            .flags = (v3_bool(c, "lsb_first", false) ? SPI_DEVICE_BIT_LSBFIRST : 0) |
                     (v3_bool(c, "half_duplex", false) ? SPI_DEVICE_HALFDUPLEX : 0)};
        spi_device_handle_t dev = NULL;
        err = spi_bus_add_device(SPI2_HOST, &d, &dev);
        h->handle = dev;
        return err;
    }
    if (LV_HAS_BUS && h->kind == V_TWAI) {
        h->secondary = calloc(1, sizeof(bus_state));
        h->events = xQueueCreate(16, sizeof(v3_packet));
        h->done = xSemaphoreCreateBinary();
        if (!h->secondary || !h->events || !h->done)
            return ESP_ERR_NO_MEM;
        const char *mode = v3_str(c, "mode", "normal");
        twai_onchip_node_config_t n = {.io_cfg = {.tx = v3_int(p, "tx", -1),
                                                  .rx = v3_int(p, "rx", -1),
                                                  .quanta_clk_out = -1,
                                                  .bus_off_indicator = -1},
                                       .bit_timing = {.bitrate = v3_int(c, "bitrate", 500000)},
                                       .tx_queue_depth = v3_int(c, "queue_depth", 4),
                                       .fail_retry_cnt = 0,
                                       .flags = {.enable_listen_only = !strcmp(mode, "listen_only"),
                                                 .enable_self_test = !strcmp(mode, "self_test"),
                                                 .enable_loopback = !strcmp(mode, "self_test")}};
        twai_node_handle_t node = NULL;
        if ((err = twai_new_node_onchip(&n, &node)))
            return err;
        h->handle = node;
        twai_event_callbacks_t cb = {.on_rx_done = can_received, .on_tx_done = can_sent};
        if ((err = twai_node_register_event_callbacks(node, &cb, h)))
            return err;
        twai_mask_filter_config_t filter = {.id = v3_int(c, "filter_id", 0),
                                            .mask = v3_int(c, "filter_mask", 0),
                                            .is_ext = v3_bool(c, "extended", false)};
        if ((err = twai_node_config_mask_filter(node, 0, &filter)))
            return err;
        err = twai_node_enable(node);
        if (!err)
            h->enabled = true;
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
static int spi_drain(v3_hardware *h, int timeout) {
    bus_state *s = h->secondary;
    if (!s || !s->pending)
        return ESP_OK;
    int err;
    if (LV_HAS_BUS && h->kind == V_SPI_DEVICE) {
        spi_transaction_t *t = NULL;
        err = spi_device_get_trans_result(h->handle, &t, pdMS_TO_TICKS(timeout));
    } else {
        spi_slave_transaction_t *t = NULL;
        err = spi_slave_get_trans_result(SPI2_HOST, &t, pdMS_TO_TICKS(timeout));
    }
    if (!err)
        s->pending = false;
    return err;
}
int v3_bus_close(v3_hardware *h) {
    int err = ESP_OK;
    if (LV_HAS_BUS && h->kind == V_UART && h->installed) {
        if ((err = uart_driver_delete(h->controller)))
            return err;
        h->installed = false;
    }
    if (h->kind == V_I2C_DEVICE && h->handle) {
        if ((err = i2c_master_bus_rm_device(h->handle)))
            return err;
        h->handle = NULL;
    }
    if (h->kind == V_I2C && h->handle) {
        if ((err = i2c_del_master_bus(h->handle)))
            return err;
        h->handle = NULL;
    }
    if (LV_HAS_SLAVE && h->kind == V_I2C_SLAVE && h->handle) {
        if ((err = i2c_del_slave_device(h->handle)))
            return err;
        h->handle = NULL;
    }
    if (LV_HAS_BUS && h->kind == V_SPI_DEVICE && h->handle) {
        if ((err = spi_drain(h, 100)))
            return err;
        if ((err = spi_bus_remove_device(h->handle)))
            return err;
        h->handle = NULL;
    }
    if (LV_HAS_BUS && h->kind == V_SPI && h->installed) {
        if ((err = spi_bus_free(SPI2_HOST)))
            return err;
        h->installed = false;
    }
    if (LV_HAS_SLAVE && h->kind == V_SPI_SLAVE && h->installed) {
        bus_state *s = h->secondary;
        if (s->ready >= 0)
            gpio_set_level(s->ready, 0);
        if (!s->slave_disabled) {
            if ((err = spi_slave_disable(SPI2_HOST)))
                return err;
            s->slave_disabled = true;
        }
        if ((err = spi_slave_free(SPI2_HOST)))
            return err;
        h->installed = false;
        s->pending = false;
    }
    if (LV_HAS_BUS && h->kind == V_TWAI && h->handle) {
        if (h->enabled) {
            if ((err = twai_node_disable(h->handle)))
                return err;
            h->enabled = false;
        }
        if ((err = twai_node_delete(h->handle)))
            return err;
        h->handle = NULL;
    }
    free(h->secondary);
    h->secondary = NULL;
    return ESP_OK;
}
static int uart_call(v3_hardware *h, const v3_work *w, cJSON **out, bool *external) {
    const char *a = w->action;
    const cJSON *p = w->params;
    int timeout = v3_timeout(p);
    if (timeout < 0)
        return ESP_ERR_INVALID_ARG;
    int64_t until = esp_timer_get_time() / 1000 + timeout;
    int err;
    size_t n = 0;
    if (!strcmp(a, "write") || !strcmp(a, "break")) {
        if (!v3_get(v3_hw_pins(h), "tx"))
            return ESP_ERR_INVALID_ARG;
        if (!strcmp(a, "break")) {
            // The public BREAK API waits for transmission; CTS may block indefinitely.
            if (v3_bool(v3_hw_config(h), "flow_control", false))
                return ESP_ERR_NOT_SUPPORTED;
            if (!v3_integer(v3_get(p, "bits"), 1, 255))
                return ESP_ERR_INVALID_ARG;
            // An empty payload cannot trigger the public break API; use an explicit final data
            // byte.
            if (v3_payload(p, h->memory, V3_BUFFER_MAX, &n) || !n)
                return ESP_ERR_INVALID_ARG;
            // IDF break write is bounded by the maximum configured wire duration below.
            if (n > 128 || ((double)(n * 12 + v3_int(p, "bits", 0)) * 1000 /
                            v3_int(v3_hw_config(h), "baud", 115200)) > 100)
                return ESP_ERR_INVALID_ARG;
            int sent =
                uart_write_bytes_with_break(h->controller, h->memory, n, v3_int(p, "bits", 1));
            *external = sent > 0;
            return sent == (int)n ? ESP_OK : ESP_FAIL;
        }
        if (v3_payload(p, h->memory, v3_int(v3_hw_config(h), "tx_buffer", 4096), &n))
            return ESP_ERR_INVALID_ARG;
        size_t sent = 0;
        do {
            int k = uart_tx_chars(h->controller, (const char *)h->memory + sent, n - sent);
            if (k > 0) {
                sent += k;
                *external = true;
            }
            if (sent == n)
                break;
            if (v3_cancelled(w))
                return ESP_ERR_INVALID_STATE;
            vTaskDelay(1);
        } while (esp_timer_get_time() / 1000 < until);
        *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(*out, "written", sent);
        return sent == n ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    if (!strcmp(a, "read")) {
        if (!v3_get(v3_hw_pins(h), "rx") && !v3_bool(v3_hw_config(h), "loopback", false))
            return ESP_ERR_INVALID_ARG;
        if (!v3_integer(v3_get(p, "length"), 1, V3_BUFFER_MAX))
            return ESP_ERR_INVALID_ARG;
        int k = uart_read_bytes(h->controller, h->memory, v3_int(p, "length", 1),
                                pdMS_TO_TICKS(timeout > 10 ? 10 : timeout));
        while (k == 0 && esp_timer_get_time() / 1000 < until && !v3_cancelled(w))
            k = uart_read_bytes(h->controller, h->memory, v3_int(p, "length", 1),
                                pdMS_TO_TICKS(10));
        return k < 0 ? ESP_FAIL : v3_binary_result(h->memory, k, out);
    }
    if (!strcmp(a, "drain")) {
        do {
            err = uart_wait_tx_done(h->controller, pdMS_TO_TICKS(10));
            if (!err)
                return ESP_OK;
        } while (!v3_cancelled(w) && esp_timer_get_time() / 1000 < until);
        return ESP_ERR_TIMEOUT;
    }
    if (!strcmp(a, "clear"))
        return uart_flush_input(h->controller);
    if (!strcmp(a, "errors")) {
        v3_bus_poll(h);
        *out = v3_hw_status(h);
        size_t bytes = 0;
        uart_get_buffered_data_len(h->controller, &bytes);
        cJSON_AddNumberToObject(*out, "rx_bytes", bytes);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
void v3_bus_poll(v3_hardware *h) {
    if (!LV_HAS_BUS || h->kind != V_UART || !h->installed)
        return;
    bus_state *s = h->secondary;
    uart_event_t event;
    for (int i = 0; i < 16 && xQueueReceive(s->uart_events, &event, 0) == pdTRUE; ++i) {
        if (event.type == UART_DATA)
            atomic_fetch_add(&h->received, event.size);
        else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL)
            atomic_fetch_add(&h->dropped, 1);
        else
            atomic_fetch_add(&h->errors, 1);
        v3_packet p = {.id = atomic_fetch_add(&h->sequence, 1) + 1,
                       .flags = event.type,
                       .time = esp_timer_get_time(),
                       .length = 4};
        uint32_t size = event.size;
        memcpy(p.data, &size, 4);
        if (xQueueSend(h->events, &p, 0) != pdTRUE)
            atomic_fetch_add(&h->dropped, 1);
    }
}
int v3_bus_call(v3_hardware *h, const v3_work *w, cJSON **out, bool *external) {
    const char *a = w->action;
    const cJSON *p = w->params, *c = v3_hw_config(h);
    int timeout = v3_timeout(p), err;
    if (timeout < 0)
        return ESP_ERR_INVALID_ARG;
    if (LV_HAS_BUS && h->kind == V_UART)
        return uart_call(h, w, out, external);
    if (h->kind == V_I2C) {
        if (!strcmp(a, "recover"))
            return i2c_master_bus_reset(h->handle);
        if (!strcmp(a, "probe")) {
            if (!v3_integer(v3_get(p, "address"), 0, 127))
                return ESP_ERR_INVALID_ARG;
            return i2c_master_probe(h->handle, v3_int(p, "address", 0), timeout);
        }
        if (!strcmp(a, "scan")) {
            *out = cJSON_CreateObject();
            cJSON *list = cJSON_AddArrayToObject(*out, "addresses");
            for (int address = 8; address < 120; ++address) {
                if (v3_cancelled(w))
                    return ESP_ERR_INVALID_STATE;
                err = i2c_master_probe(h->handle, address, timeout < 10 ? timeout : 10);
                if (err == ESP_OK)
                    v3_push(list, cJSON_CreateNumber(address));
                else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_TIMEOUT)
                    return err;
            }
            return ESP_OK;
        }
    }
    if (h->kind == V_I2C_DEVICE) {
        size_t n = 0;
        int length = v3_int(p, "length", 0);
        if (length < 0 || length > V3_BUFFER_MAX)
            return ESP_ERR_INVALID_ARG;
        if (!strcmp(a, "read")) {
            if (!length)
                return ESP_ERR_INVALID_ARG;
            err = i2c_master_receive(h->handle, h->memory, length, timeout);
            return err ? err : v3_binary_result(h->memory, length, out);
        }
        if (!strcmp(a, "write") || !strcmp(a, "write_read")) {
            if (v3_payload(p, h->memory, V3_BUFFER_MAX, &n) || !n)
                return ESP_ERR_INVALID_ARG;
            *external = true;
            if (!strcmp(a, "write"))
                return i2c_master_transmit(h->handle, h->memory, n, timeout);
            if (!length)
                return ESP_ERR_INVALID_ARG;
            err = i2c_master_transmit_receive(h->handle, h->memory, n, h->memory + V3_BUFFER_MAX,
                                              length, timeout);
            return err ? err : v3_binary_result(h->memory + V3_BUFFER_MAX, length, out);
        }
    }
    if (LV_HAS_SLAVE && h->kind == V_I2C_SLAVE) {
        if (!strcmp(a, "preload")) {
            size_t n;
            uint32_t written = 0;
            if (v3_payload(p, h->memory, V3_BUFFER_MAX, &n))
                return ESP_ERR_INVALID_ARG;
            err = i2c_slave_write(h->handle, h->memory, n, &written, timeout);
            *out = cJSON_CreateObject();
            cJSON_AddNumberToObject(*out, "written", written);
            return err;
        }
        if (!strcmp(a, "clear_tx"))
            return i2c_slave_reset_tx_fifo(h->handle);
    }
    if ((LV_HAS_BUS && h->kind == V_SPI_DEVICE) || (LV_HAS_SLAVE && h->kind == V_SPI_SLAVE)) {
        bus_state *s = h->secondary;
        if (!strcmp(a, "transfer") || !strcmp(a, "queue")) {
            if (s->pending)
                return ESP_ERR_INVALID_STATE;
            if (h->kind == V_SPI_DEVICE) {
                v3_hardware *parent = v3_hw_parent(h);
                for (int i = 0; i < V3_RESOURCES; ++i) {
                    if (!(atomic_load(&v3_lane_slots[4]) & (1u << i)))
                        continue;
                    v3_hardware *other = &v3_hardware_slots[i];
                    if (other != h && other->kind == V_SPI_DEVICE && other->id &&
                        v3_hw_parent(other) == parent && spi_drain(other, 0) != ESP_OK)
                        return ESP_ERR_INVALID_STATE;
                }
            }
            size_t n = 0;
            int rx = v3_int(p, "rx_length", 0);
            if (rx < 0 || rx > V3_BUFFER_MAX)
                return ESP_ERR_INVALID_ARG;
            if (v3_get(p, "data") && v3_payload(p, h->memory, V3_BUFFER_MAX, &n))
                return ESP_ERR_INVALID_ARG;
            size_t len = n > (size_t)rx ? n : (size_t)rx;
            if (!len || len > (size_t)v3_int(c, "max_transfer", 4096))
                return ESP_ERR_INVALID_ARG;
            memset(h->memory + n, 0, V3_BUFFER_MAX - n);
            memset(h->memory + V3_BUFFER_MAX, 0, V3_BUFFER_MAX);
            if (LV_HAS_BUS && h->kind == V_SPI_DEVICE) {
                if (!strcmp(a, "queue"))
                    return ESP_ERR_INVALID_ARG;
                // A bounded wire duration is required before hardware admission.
                if ((len * 8.0 + v3_int(c, "command_bits", 0) + v3_int(c, "address_bits", 0) +
                     v3_int(c, "dummy_bits", 0)) *
                        1000 / v3_int(c, "frequency", 1000000) >
                    100)
                    return ESP_ERR_INVALID_ARG;
                memset(&s->master, 0, sizeof(s->master));
                spi_transaction_t *t = &s->master.base;
                bool half = v3_bool(c, "half_duplex", false);
                t->length = half ? n * 8 : len * 8;
                t->rxlength = rx * 8;
                t->tx_buffer = n ? h->memory : NULL;
                t->rx_buffer = rx ? h->memory + V3_BUFFER_MAX : NULL;
                if (half && n && rx)
                    return ESP_ERR_NOT_SUPPORTED; // C3 DMA half-duplex write+read requires separate
                                                  // transactions.
                t->cmd = v3_int(p, "command", 0);
                const char *addr = v3_str(p, "address_hex", "");
                if (strlen(addr) > 16)
                    return ESP_ERR_INVALID_ARG;
                char *end = NULL;
                t->addr = strtoull(addr, &end, 16);
                if (*addr && (!end || *end))
                    return ESP_ERR_INVALID_ARG;
                int lines = v3_int(p, "lines", 1);
                if (lines != 1 && lines != 2 && lines != 4)
                    return ESP_ERR_INVALID_ARG;
                if (lines == 2)
                    t->flags |= SPI_TRANS_MODE_DIO;
                if (lines == 4)
                    t->flags |= SPI_TRANS_MODE_QIO;
                if (lines > 1 && !half)
                    return ESP_ERR_INVALID_ARG;
                err = spi_device_queue_trans(h->handle, t, pdMS_TO_TICKS(timeout));
            } else {
                memset(&s->slave, 0, sizeof(s->slave));
                s->slave.length = len * 8;
                s->slave.tx_buffer = h->memory;
                s->slave.rx_buffer = h->memory + V3_BUFFER_MAX;
                s->slave.user = h;
                if (s->slave_disabled) {
                    if ((err = spi_slave_enable(SPI2_HOST)))
                        return err;
                    s->slave_disabled = false;
                }
                err = spi_slave_queue_trans(SPI2_HOST, &s->slave, pdMS_TO_TICKS(timeout));
            }
            if (err)
                return err;
            s->pending = true;
            *external = true;
            if (LV_HAS_SLAVE && h->kind == V_SPI_SLAVE) {
                *out = cJSON_CreateObject();
                cJSON_AddBoolToObject(*out, "queued", true);
                return ESP_OK;
            }
            if ((err = spi_drain(h, timeout)))
                return err;
            return v3_binary_result(h->memory + V3_BUFFER_MAX, rx, out);
        }
        if (!strcmp(a, "result")) {
            if ((err = spi_drain(h, timeout)))
                return err;
            size_t bytes =
                h->kind == V_SPI_SLAVE ? (s->slave.trans_len + 7) / 8 : s->master.base.rxlength / 8;
            if (bytes > V3_BUFFER_MAX)
                bytes = V3_BUFFER_MAX;
            err = v3_binary_result(h->memory + V3_BUFFER_MAX, bytes, out);
            if (!err && h->kind == V_SPI_SLAVE)
                cJSON_AddNumberToObject(*out, "actual_bits", s->slave.trans_len);
            return err;
        }
        if (!strcmp(a, "cancel") && h->kind == V_SPI_SLAVE) {
            err = spi_slave_disable(SPI2_HOST);
            if (!err) {
                s->slave_disabled = true;
                if (s->ready >= 0)
                    gpio_set_level(s->ready, 0);
            }
            return err;
        }
    }
    if (LV_HAS_BUS && h->kind == V_TWAI) {
        bus_state *s = h->secondary;
        if (!strcmp(a, "recover"))
            return twai_node_recover(h->handle);
        if (!strcmp(a, "info")) {
            twai_node_status_t status;
            twai_node_record_t record;
            if ((err = twai_node_get_info(h->handle, &status, &record)))
                return err;
            *out = v3_hw_status(h);
            cJSON_AddNumberToObject(*out, "bus_state", status.state);
            cJSON_AddNumberToObject(*out, "tx_errors", status.tx_error_count);
            cJSON_AddNumberToObject(*out, "rx_errors", status.rx_error_count);
            cJSON_AddNumberToObject(*out, "bus_errors", record.bus_err_num);
            return ESP_OK;
        }
        if (!strcmp(a, "send")) {
            if (s->pending && xSemaphoreTake(h->done, 0) != pdTRUE)
                return ESP_ERR_INVALID_STATE;
            s->pending = false;
            bool ext = v3_bool(p, "extended", false);
            if (!v3_integer(v3_get(p, "id"), 0, ext ? 536870911 : 2047))
                return ESP_ERR_INVALID_ARG;
            size_t n = 0;
            if (v3_payload(p, s->can_data, 8, &n))
                return ESP_ERR_INVALID_ARG;
            s->can = (twai_frame_t){.header = {.id = v3_int(p, "id", 0),
                                               .dlc = n,
                                               .ide = ext,
                                               .rtr = v3_bool(p, "remote", false)},
                                    .buffer = s->can_data,
                                    .buffer_len = n};
            xSemaphoreTake(h->done, 0);
            if ((err = twai_node_transmit(h->handle, &s->can, timeout)))
                return err;
            s->pending = true;
            *external = true;
            int64_t until = esp_timer_get_time() / 1000 + timeout;
            do {
                if (xSemaphoreTake(h->done, pdMS_TO_TICKS(10)) == pdTRUE) {
                    s->pending = false;
                    return atomic_load(&h->actual) == 1 ? ESP_OK : ESP_FAIL;
                }
            } while (!v3_cancelled(w) && esp_timer_get_time() / 1000 < until);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;
}
