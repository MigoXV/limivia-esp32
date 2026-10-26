#include "peripherals.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_pdm.h"
#include "driver/sdm.h"
#include "esp_adc/adc_continuous.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
static rmt_sync_manager_handle_t pending_sync;
static v3_hardware *sync_members[2];
static int release_sync(void) {
    if (!pending_sync)
        return ESP_OK;
    for (int i = 0; i < 2; ++i)
        if (sync_members[i]->enabled) {
            int err = rmt_disable(sync_members[i]->handle);
            if (err)
                return err;
            sync_members[i]->enabled = false;
            sync_members[i]->installed = false;
            v3_realtime_end(sync_members[i]);
        }
    int err = rmt_del_sync_manager(pending_sync);
    if (err)
        return err;
    pending_sync = NULL;
    sync_members[0] = sync_members[1] = NULL;
    return ESP_OK;
}
int v3_rmt_sync(const v3_work *w, cJSON **out, bool *external) {
    if (pending_sync)
        return ESP_ERR_INVALID_STATE;
    const cJSON *slots = v3_get(w->params, "slots"), *resources = v3_get(w->params, "resources");
    rmt_channel_handle_t channels[2];
    v3_hardware *members[2];
    for (int i = 0; i < 2; ++i) {
        int slot = cJSON_GetArrayItem(slots, i)->valueint;
        if (slot < 0 || slot >= V3_RESOURCES)
            return ESP_ERR_INVALID_ARG;
        v3_hardware *h = &v3_hardware_slots[slot];
        members[i] = h;
        if (h->id != (uint32_t)v3_int(cJSON_GetArrayItem(resources, i), "resource", 0) ||
            h->instance != (uint32_t)v3_int(cJSON_GetArrayItem(resources, i), "instance", 0) ||
            h->kind != V_RMT_TX || !h->handle)
            return ESP_ERR_INVALID_STATE;
        if (h->installed && rmt_tx_wait_all_done(h->handle, 0) != ESP_OK)
            return ESP_ERR_INVALID_STATE;
        channels[i] = h->handle;
    }
    size_t lengths[2];
    for (int i = 0; i < 2; ++i) {
        v3_hardware *h = members[i];
        int err;
        if (v3_payload(cJSON_GetArrayItem(resources, i), h->memory, V3_BUFFER_MAX, &lengths[i]) ||
            !lengths[i] || lengths[i] % 4)
            return ESP_ERR_INVALID_ARG;
        rmt_symbol_word_t *symbols = (void *)h->memory;
        for (size_t j = 0; j < lengths[i] / 4; ++j)
            if (!symbols[j].duration0 || !symbols[j].duration1)
                return ESP_ERR_INVALID_ARG;
        if (!h->enabled) {
            if ((err = rmt_enable(h->handle)))
                return err;
            h->enabled = true;
        }
    }
    rmt_sync_manager_config_t cfg = {.tx_channel_array = channels, .array_size = 2};
    int err = rmt_new_sync_manager(&cfg, &pending_sync);
    if (err)
        return err;
    sync_members[0] = members[0];
    sync_members[1] = members[1];
    for (int i = 0; i < 2; ++i) {
        v3_hardware *h = members[i];
        if ((err = v3_realtime_begin(h)))
            break;
        rmt_transmit_config_t t = {.loop_count = 0,
                                   .flags.queue_nonblocking = true,
                                   .flags.eot_level = v3_int(v3_hw_config(h), "idle_level", 0)};
        if ((err = rmt_transmit(h->handle, h->encoder, h->memory, lengths[i], &t)))
            break;
        h->installed = true;
        *external = true;
    }
    int timeout = v3_timeout(w->params);
    if (timeout < 0)
        err = ESP_ERR_INVALID_ARG;
    int64_t until = esp_timer_get_time() / 1000 + (timeout < 0 ? 0 : timeout);
    while (!err) {
        bool done = true;
        for (int i = 0; i < 2; ++i)
            if (rmt_tx_wait_all_done(channels[i], 0) != ESP_OK)
                done = false;
        if (done)
            break;
        if (v3_cancelled(w)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (esp_timer_get_time() / 1000 >= until) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(1);
    }
    int cleanup = release_sync();
    if (cleanup)
        return cleanup;
    if (!err) {
        *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(*out, "synchronized", true);
    }
    return err;
}

static bool rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *e, void *ctx) {
    (void)ch;
    v3_hardware *h = ctx;
    atomic_store(&h->actual, e->num_symbols * sizeof(rmt_symbol_word_t));
    atomic_fetch_add(&h->sequence, 1);
    atomic_fetch_add(&h->received, 1);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(h->done, &wake);
    return wake == pdTRUE;
}
static bool adc_overflow(adc_continuous_handle_t adc, const adc_continuous_evt_data_t *e,
                         void *ctx) {
    (void)adc;
    (void)e;
    v3_hardware *h = ctx;
    atomic_fetch_add(&h->dropped, 1);
    return false;
}
static int i2s_open(v3_hardware *h) {
    const cJSON *c = v3_hw_config(h), *p = v3_hw_pins(h);
    int err;
    const char *dir = v3_str(c, "direction", "duplex"), *mode = v3_str(c, "mode", "standard");
    bool tx = strcmp(dir, "rx") != 0, rx = strcmp(dir, "tx") != 0;
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0,
        !strcmp(v3_str(c, "role", "master"), "master") ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    ch.dma_frame_num = v3_int(c, "dma_frames", 128);
    ch.dma_desc_num = 4;
    ch.auto_clear = true;
    i2s_chan_handle_t txh = NULL, rxh = NULL;
    if ((err = i2s_new_channel(&ch, tx ? &txh : NULL, rx ? &rxh : NULL)))
        return err;
    h->handle = txh;
    h->secondary = rxh;
    int rate = v3_int(c, "sample_rate", 16000), bits = v3_int(c, "bits", 16),
        slots = v3_int(c, "slots", 2);
    i2s_slot_mode_t sm = slots == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    if (!strcmp(mode, "standard")) {
        i2s_std_config_t s = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
                              .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, sm),
                              .gpio_cfg = {.mclk = v3_int(p, "mclk", -1),
                                           .bclk = v3_int(p, "bclk", -1),
                                           .ws = v3_int(p, "ws", -1),
                                           .dout = v3_int(p, "dout", -1),
                                           .din = v3_int(p, "din", -1)}};
        if (bits == 24)
            s.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
        s.slot_cfg.bit_shift = v3_bool(c, "bit_shift", true);
        if (tx && (err = i2s_channel_init_std_mode(txh, &s)))
            return err;
        if (rx && (err = i2s_channel_init_std_mode(rxh, &s)))
            return err;
    } else if (!strcmp(mode, "tdm")) {
        i2s_tdm_config_t s = {
            .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(rate),
            .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bits, sm, v3_int(c, "slot_mask", 3)),
            .gpio_cfg = {.mclk = v3_int(p, "mclk", -1),
                         .bclk = v3_int(p, "bclk", -1),
                         .ws = v3_int(p, "ws", -1),
                         .dout = v3_int(p, "dout", -1),
                         .din = v3_int(p, "din", -1)}};
        s.slot_cfg.total_slot = slots;
        s.slot_cfg.left_align = v3_bool(c, "left_align", false);
        s.slot_cfg.bit_shift = v3_bool(c, "bit_shift", true);
        if (bits == 24)
            s.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
        if (tx && (err = i2s_channel_init_tdm_mode(txh, &s)))
            return err;
        if (rx && (err = i2s_channel_init_tdm_mode(rxh, &s)))
            return err;
    } else if (tx) {
        i2s_pdm_tx_config_t s = {
            .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(rate),
            .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(bits, sm),
            .gpio_cfg = {.clk = v3_int(p, "bclk", -1), .dout = v3_int(p, "dout", -1)}};
        if ((err = i2s_channel_init_pdm_tx_mode(txh, &s)))
            return err;
    } else {
        i2s_pdm_rx_config_t s = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate),
            .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(bits, sm),
            .gpio_cfg = {.clk = v3_int(p, "bclk", -1), .din = v3_int(p, "din", -1)}};
        if ((err = i2s_channel_init_pdm_rx_mode(rxh, &s)))
            return err;
    }
    if ((err = v3_realtime_begin(h)))
        return err;
    if (tx) {
        if ((err = i2s_channel_enable(txh)))
            return err;
        h->tx_enabled = true;
    }
    if (rx) {
        if ((err = i2s_channel_enable(rxh)))
            return err;
        h->rx_enabled = true;
    }
    return ESP_OK;
}
int v3_signal_open(v3_hardware *h, const v3_work *w) {
    (void)w;
    const cJSON *c = v3_hw_config(h), *p = v3_hw_pins(h);
    int err;
    h->memory_size = V3_BUFFER_MAX;
    h->memory = calloc(1, h->memory_size);
    if (!h->memory)
        return ESP_ERR_NO_MEM;
    h->done = xSemaphoreCreateBinary();
    if (!h->done)
        return ESP_ERR_NO_MEM;
    if (h->kind == V_RMT_TX || h->kind == V_RMT_RX) {
        rmt_channel_handle_t ch = NULL;
        if (h->kind == V_RMT_TX) {
            rmt_tx_channel_config_t t = {.gpio_num = v3_int(p, "pin", -1),
                                         .clk_src = RMT_CLK_SRC_DEFAULT,
                                         .resolution_hz = v3_int(c, "resolution_hz", 1000000),
                                         .mem_block_symbols = 48,
                                         .trans_queue_depth = 1,
                                         .flags.invert_out = v3_bool(c, "invert", false),
                                         .flags.init_level = v3_int(c, "idle_level", 0)};
            if ((err = rmt_new_tx_channel(&t, &ch)))
                return err;
            h->handle = ch;
            rmt_copy_encoder_config_t e = {};
            rmt_encoder_handle_t encoder = NULL;
            if ((err = rmt_new_copy_encoder(&e, &encoder)))
                return err;
            h->encoder = encoder;
        } else {
            rmt_rx_channel_config_t r = {.gpio_num = v3_int(p, "pin", -1),
                                         .clk_src = RMT_CLK_SRC_DEFAULT,
                                         .resolution_hz = v3_int(c, "resolution_hz", 1000000),
                                         .mem_block_symbols = 48,
                                         .flags.invert_in = v3_bool(c, "invert", false)};
            if ((err = rmt_new_rx_channel(&r, &ch)))
                return err;
            h->handle = ch;
            rmt_rx_event_callbacks_t cb = {.on_recv_done = rx_done};
            if ((err = rmt_rx_register_event_callbacks(ch, &cb, h)))
                return err;
        }
        int carrier = v3_int(c, "carrier_hz", 0);
        if (carrier) {
            rmt_carrier_config_t cc = {.frequency_hz = carrier,
                                       .duty_cycle = v3_num(c, "carrier_duty", 0.33)};
            if ((err = rmt_apply_carrier(ch, &cc)))
                return err;
        }
        if ((err = rmt_enable(ch)))
            return err;
        h->enabled = true;
        return ESP_OK;
    }
    if (LV_HAS_SIGNAL && h->kind == V_ADC_CONT) {
        adc_continuous_handle_cfg_t hc = {.max_store_buf_size = v3_int(c, "pool_bytes", 4096),
                                          .conv_frame_size = 256,
                                          .flags.flush_pool = false};
        adc_continuous_handle_t adc = NULL;
        if ((err = adc_continuous_new_handle(&hc, &adc)))
            return err;
        h->handle = adc;
        adc_digi_pattern_config_t pattern[4];
        int n = 0;
        for (int pin = 0; pin <= 4; ++pin)
            if (h->pins & (1u << pin))
                pattern[n++] = (adc_digi_pattern_config_t){.atten = v3_int(c, "attenuation", 3),
                                                           .channel = pin,
                                                           .unit = ADC_UNIT_1,
                                                           .bit_width = 12};
        adc_continuous_config_t conf = {.sample_freq_hz = v3_int(c, "sample_rate", 10000),
                                        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
                                        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
                                        .pattern_num = n,
                                        .adc_pattern = pattern};
        if ((err = adc_continuous_config(adc, &conf)))
            return err;
        adc_continuous_evt_cbs_t cb = {.on_pool_ovf = adc_overflow};
        if ((err = adc_continuous_register_event_callbacks(adc, &cb, h)))
            return err;
        h->events = xQueueCreate(8, sizeof(v3_packet));
        return h->events ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (LV_HAS_SIGNAL && h->kind == V_I2S)
        return i2s_open(h);
    if (LV_HAS_SIGNAL && h->kind == V_SDM) {
        sdm_config_t s = {.gpio_num = v3_int(p, "pin", -1),
                          .clk_src = SDM_CLK_SRC_DEFAULT,
                          .sample_rate_hz = v3_int(c, "sample_rate", 1000000),
                          .flags.invert_out = v3_bool(c, "invert", false)};
        sdm_channel_handle_t ch = NULL;
        if ((err = sdm_new_channel(&s, &ch)))
            return err;
        h->handle = ch;
        if ((err = sdm_channel_set_pulse_density(ch, v3_int(c, "density", 0))))
            return err;
        if ((err = sdm_channel_enable(ch)))
            return err;
        h->enabled = true;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
int v3_signal_close(v3_hardware *h) {
    int err;
    if (pending_sync && (sync_members[0] == h || sync_members[1] == h)) {
        if ((err = release_sync()))
            return err;
    }
    if ((h->kind == V_RMT_TX || h->kind == V_RMT_RX) && h->handle) {
        if (h->enabled) {
            if ((err = rmt_disable(h->handle)))
                return err;
            h->enabled = false;
        }
        if ((err = rmt_del_channel(h->handle)))
            return err;
        h->handle = NULL;
        if (h->encoder) {
            if ((err = rmt_del_encoder(h->encoder)))
                return err;
            h->encoder = NULL;
        }
    }
    if (LV_HAS_SIGNAL && h->kind == V_ADC_CONT && h->handle) {
        if (h->enabled) {
            if ((err = adc_continuous_stop(h->handle)))
                return err;
            h->enabled = false;
        }
        if ((err = adc_continuous_deinit(h->handle)))
            return err;
        h->handle = NULL;
    }
    if (LV_HAS_SIGNAL && h->kind == V_SDM && h->handle) {
        if (h->enabled) {
            if ((err = sdm_channel_disable(h->handle)))
                return err;
            h->enabled = false;
        }
        if ((err = sdm_del_channel(h->handle)))
            return err;
        h->handle = NULL;
    }
    if (LV_HAS_SIGNAL && h->kind == V_I2S) {
        if (h->handle) {
            if (h->tx_enabled) {
                if ((err = i2s_channel_disable(h->handle)))
                    return err;
                h->tx_enabled = false;
            }
            if ((err = i2s_del_channel(h->handle)))
                return err;
            h->handle = NULL;
        }
        if (h->secondary) {
            if (h->rx_enabled) {
                if ((err = i2s_channel_disable(h->secondary)))
                    return err;
                h->rx_enabled = false;
            }
            if ((err = i2s_del_channel(h->secondary)))
                return err;
            h->secondary = NULL;
        }
    }
    return ESP_OK;
}
int v3_signal_call(v3_hardware *h, const v3_work *w, cJSON **out, bool *external) {
    const cJSON *p = w->params;
    const char *a = w->action;
    int err, timeout = v3_timeout(p);
    if (timeout < 0)
        return ESP_ERR_INVALID_ARG;
    if (LV_HAS_SIGNAL && h->kind == V_ADC_CONT) {
        if (!strcmp(a, "start")) {
            if (h->enabled)
                return ESP_ERR_INVALID_STATE;
            if ((err = v3_realtime_begin(h)))
                return err;
            err = adc_continuous_start(h->handle);
            if (!err)
                h->enabled = true;
            else
                v3_realtime_end(h);
            return err;
        }
        if (!strcmp(a, "stop")) {
            if (!h->enabled)
                return ESP_OK;
            err = adc_continuous_stop(h->handle);
            if (!err)
                h->enabled = false;
            if (!err)
                v3_realtime_end(h);
            return err;
        }
        if (!strcmp(a, "read")) {
            if (h->enabled)
                return ESP_ERR_INVALID_STATE; // Stop capture first, then consume the finite
                                              // retained events.
            *out = v3_hw_status(h);
            cJSON_AddStringToObject(*out, "format", "esp32c3_adc_type2_le");
            return ESP_OK;
        }
    }
    if (LV_HAS_SIGNAL && h->kind == V_SDM && !strcmp(a, "density")) {
        if (!v3_integer(v3_get(p, "density"), -128, 127))
            return ESP_ERR_INVALID_ARG;
        return sdm_channel_set_pulse_density(h->handle, v3_int(p, "density", 0));
    }
    if (h->kind == V_RMT_TX) {
        if (!strcmp(a, "abort")) {
            err = rmt_disable(h->handle);
            if (!err)
                h->enabled = false;
            if (!err)
                v3_realtime_end(h);
            return err;
        }
        if (!strcmp(a, "transmit")) {
            if (!h->enabled) {
                if ((err = rmt_enable(h->handle)))
                    return err;
                h->enabled = true;
            }
            // A previous asynchronous/looped transaction must complete before its payload is
            // reused.
            if (h->installed && rmt_tx_wait_all_done(h->handle, 0) != ESP_OK)
                return ESP_ERR_INVALID_STATE;
            size_t n;
            if (v3_payload(p, h->memory, V3_BUFFER_MAX, &n) || !n || n % 4)
                return ESP_ERR_INVALID_ARG;
            rmt_symbol_word_t *s = (void *)h->memory;
            for (size_t i = 0; i < n / 4; ++i)
                if (!s[i].duration0 || !s[i].duration1)
                    return ESP_ERR_INVALID_ARG;
            if (v3_get(p, "loops") && !v3_integer(v3_get(p, "loops"), -1, 1023))
                return ESP_ERR_INVALID_ARG;
            rmt_transmit_config_t t = {.loop_count = v3_int(p, "loops", 0),
                                       .flags.eot_level = v3_int(v3_hw_config(h), "idle_level", 0),
                                       .flags.queue_nonblocking = true};
            if ((err = v3_realtime_begin(h)))
                return err;
            if ((err = rmt_transmit(h->handle, h->encoder, h->memory, n, &t))) {
                v3_realtime_end(h);
                return err;
            }
            h->installed = true;
            *external = true;
            *out = cJSON_CreateObject();
            cJSON_AddBoolToObject(*out, "queued", true);
            return ESP_OK;
        }
        if (!strcmp(a, "wait")) {
            int64_t until = esp_timer_get_time() / 1000 + timeout;
            do {
                err = rmt_tx_wait_all_done(h->handle, 10);
                if (!err)
                    return ESP_OK;
            } while (!v3_cancelled(w) && esp_timer_get_time() / 1000 < until);
            return ESP_ERR_TIMEOUT;
        }
    }
    if (h->kind == V_RMT_RX) {
        if (!strcmp(a, "capture")) {
            if (h->installed && xSemaphoreTake(h->done, 0) != pdTRUE)
                return ESP_ERR_INVALID_STATE;
            xSemaphoreTake(h->done, 0);
            atomic_store(&h->actual, 0);
            if (!v3_integer(v3_get(p, "max_ns"), 1, 4294967295.0))
                return ESP_ERR_INVALID_ARG;
            rmt_receive_config_t r = {.signal_range_min_ns = v3_int(p, "min_ns", 0),
                                      .signal_range_max_ns =
                                          (uint32_t)v3_num(p, "max_ns", 1000000)};
            if ((err = v3_realtime_begin(h)))
                return err;
            err = rmt_receive(h->handle, h->memory, V3_BUFFER_MAX, &r);
            if (!err)
                h->installed = true;
            else
                v3_realtime_end(h);
            return err;
        }
        if (!strcmp(a, "result")) {
            if (xSemaphoreTake(h->done, pdMS_TO_TICKS(timeout)) != pdTRUE)
                return ESP_ERR_TIMEOUT;
            h->installed = false;
            v3_realtime_end(h);
            return v3_binary_result(h->memory, atomic_load(&h->actual), out);
        }
        if (!strcmp(a, "abort")) {
            if ((err = rmt_disable(h->handle)))
                return err;
            h->enabled = false;
            h->installed = false;
            v3_realtime_end(h);
            if ((err = rmt_enable(h->handle)))
                return err;
            h->enabled = true;
            return ESP_OK;
        }
    }
    if (LV_HAS_SIGNAL && h->kind == V_I2S) {
        if (!strcmp(a, "stop")) {
            if (h->tx_enabled) {
                if ((err = i2s_channel_disable(h->handle)))
                    return err;
                h->tx_enabled = false;
            }
            if (h->rx_enabled) {
                if ((err = i2s_channel_disable(h->secondary)))
                    return err;
                h->rx_enabled = false;
            }
            v3_realtime_end(h);
            return ESP_OK;
        }
        if (!strcmp(a, "start")) {
            if ((err = v3_realtime_begin(h)))
                return err;
            if (h->handle && !h->tx_enabled) {
                if ((err = i2s_channel_enable(h->handle)))
                    return err;
                h->tx_enabled = true;
            }
            if (h->secondary && !h->rx_enabled) {
                if ((err = i2s_channel_enable(h->secondary)))
                    return err;
                h->rx_enabled = true;
            }
            return ESP_OK;
        }
        size_t length = 0, used = 0;
        bool write = !strcmp(a, "play");
        if (!write && strcmp(a, "record"))
            return ESP_ERR_NOT_SUPPORTED;
        if (write) {
            if (!h->handle || v3_payload(p, h->memory, V3_BUFFER_MAX, &length))
                return ESP_ERR_INVALID_ARG;
        } else {
            if (!h->secondary || !v3_integer(v3_get(p, "length"), 1, V3_BUFFER_MAX))
                return ESP_ERR_INVALID_ARG;
            length = v3_int(p, "length", 0);
        }
        int64_t until = esp_timer_get_time() / 1000 + timeout;
        do {
            size_t n = 0;
            err = write ? i2s_channel_write(h->handle, h->memory + used, length - used, &n, 10)
                        : i2s_channel_read(h->secondary, h->memory + used, length - used, &n, 10);
            used += n;
            if (write && n)
                *external = true;
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
                return err;
        } while (used < length && !v3_cancelled(w) && esp_timer_get_time() / 1000 < until);
        if (write) {
            *out = cJSON_CreateObject();
            cJSON_AddNumberToObject(*out, "written", used);
        } else if ((err = v3_binary_result(h->memory, used, out)))
            return err;
        return used == length ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
void v3_signal_poll(v3_hardware *h, int64_t now) {
    (void)now;
    if (h->kind == V_RMT_TX && h->installed && rmt_tx_wait_all_done(h->handle, 0) == ESP_OK) {
        h->installed = false;
        v3_realtime_end(h);
    }
    if (h->kind == V_RMT_RX && atomic_load(&h->actual) > 0)
        v3_realtime_end(h);
    if (!LV_HAS_SIGNAL || h->kind != V_ADC_CONT || !h->enabled)
        return;
    uint32_t n = 0;
    int err = adc_continuous_read(h->handle, h->memory, 512, &n, 0);
    if (err == ESP_ERR_TIMEOUT)
        return;
    if (err) {
        atomic_fetch_add(&h->errors, 1);
        return;
    }
    for (uint32_t offset = 0; offset < n; offset += 128) {
        v3_packet e = {.id = atomic_fetch_add(&h->sequence, 1) + 1, .time = esp_timer_get_time()};
        e.length = n - offset < 128 ? n - offset : 128;
        memcpy(e.data, h->memory + offset, e.length);
        if (xQueueSend(h->events, &e, 0) != pdTRUE)
            atomic_fetch_add(&h->dropped, 1);
        else
            atomic_fetch_add(&h->received, 1);
    }
}
