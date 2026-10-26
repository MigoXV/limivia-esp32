#include "peripherals.h"
#include "pwm_latch.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

v3_hardware v3_hardware_slots[V3_RESOURCES];
atomic_uint v3_epoch = 1;
v3_stream_status v3_streams[V3_RESOURCES];
atomic_uint v3_lane_slots[8];
static void publish_stream(v3_hardware *h) {
    v3_stream_status *p = &v3_streams[h - v3_hardware_slots];
    atomic_store(&p->sequence, atomic_load(&h->sequence));
    atomic_store(&p->dropped, atomic_load(&h->dropped));
    atomic_store(&p->received, atomic_load(&h->received));
    atomic_store(&p->instance, h->instance);
}
static adc_oneshot_unit_handle_t adc_unit;
static int adc_users;
static bool isr_installed;
static bool fade_installed;
// PWM work is serialized on lane 0; retain the actual previous timer period.
static uint32_t pwm_timer_frequency[LEDC_TIMER_MAX];
static SemaphoreHandle_t flash_guard;
static atomic_uint realtime;
static bool flash_writing;
static esp_pm_lock_handle_t no_sleep, apb_lock;
static int power_users, power_error;
void v3_guard_init(void) {
    flash_guard = xSemaphoreCreateMutex();
    power_error = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "limivia_io", &no_sleep);
    if (!power_error)
        power_error = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "limivia_io", &apb_lock);
}
bool v3_flash_safe(void) { return atomic_load(&realtime) == 0; }
bool v3_flash_begin(void) {
    if (!flash_guard || xSemaphoreTake(flash_guard, pdMS_TO_TICKS(10)) != pdTRUE)
        return false;
    bool ok = !flash_writing && v3_flash_safe();
    if (ok)
        flash_writing = true;
    xSemaphoreGive(flash_guard);
    return ok;
}
void v3_flash_end(void) {
    xSemaphoreTake(flash_guard, portMAX_DELAY);
    flash_writing = false;
    xSemaphoreGive(flash_guard);
}
int v3_realtime_begin(v3_hardware *h) {
    xSemaphoreTake(flash_guard, portMAX_DELAY);
    int err = flash_writing ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (!err)
        atomic_fetch_or(&realtime, 1u << (h - v3_hardware_slots));
    xSemaphoreGive(flash_guard);
    return err;
}
void v3_realtime_end(v3_hardware *h) {
    atomic_fetch_and(&realtime, ~(1u << (h - v3_hardware_slots)));
}
static int power_acquire(v3_hardware *h) {
    if (power_error)
        return power_error;
    xSemaphoreTake(flash_guard, portMAX_DELAY);
    int err = ESP_OK;
    if (!power_users) {
        err = esp_pm_lock_acquire(no_sleep);
        if (!err)
            err = esp_pm_lock_acquire(apb_lock);
    }
    if (!err) {
        ++power_users;
        h->power_held = true;
    }
    xSemaphoreGive(flash_guard);
    return err;
}
static void power_release(v3_hardware *h) {
    if (!h->power_held)
        return;
    xSemaphoreTake(flash_guard, portMAX_DELAY);
    if (!--power_users) {
        esp_pm_lock_release(apb_lock);
        esp_pm_lock_release(no_sleep);
    }
    h->power_held = false;
    xSemaphoreGive(flash_guard);
}
bool v3_cancelled(const v3_work *w) {
    return w->epoch != atomic_load(&v3_epoch) || atomic_load(&w->cancelled);
}
int v3_lane(v3_kind k, int controller) {
    if (k == V_UART)
        return 1 + controller;
    if (k == V_I2C || k == V_I2C_DEVICE || k == V_I2C_SLAVE)
        return 3;
    if (k == V_SPI || k == V_SPI_DEVICE || k == V_SPI_SLAVE)
        return 4;
    if (k == V_TWAI)
        return 5;
    if (k == V_RMT_TX || k == V_RMT_RX)
        return 6;
    if (k == V_ADC_CONT || k == V_I2S || k == V_SDM)
        return 7;
    return 0;
}
const cJSON *v3_hw_config(const v3_hardware *h) { return v3_get(h->definition, "config"); }
const cJSON *v3_hw_pins(const v3_hardware *h) { return v3_get(h->definition, "pins"); }
v3_hardware *v3_hw_parent(const v3_hardware *h) {
    return h->parent_slot >= 0 && h->parent_slot < V3_RESOURCES ? &v3_hardware_slots[h->parent_slot]
                                                                : NULL;
}
int v3_timeout(const cJSON *p) {
    const cJSON *t = v3_get(p, "timeout_ms");
    if (t && !v3_integer(t, 0, V3_TIMEOUT_MAX))
        return -1;
    return v3_int(p, "timeout_ms", 100);
}
int v3_payload(const cJSON *p, uint8_t *bytes, size_t cap, size_t *n) {
    const char *s = v3_str(p, "data", NULL);
    return s && v3_decode64(s, bytes, cap, n) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
int v3_binary_result(const uint8_t *bytes, size_t n, cJSON **result) {
    char *s = v3_encode64(bytes, n);
    if (!s)
        return ESP_ERR_NO_MEM;
    *result = cJSON_CreateObject();
    if (!*result) {
        free(s);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(*result, "data", s);
    cJSON_AddNumberToObject(*result, "length", n);
    free(s);
    return ESP_OK;
}
static void gpio_edge(void *arg) {
    v3_hardware *h = arg;
    int64_t now = esp_timer_get_time();
    // debounce is copied to phase at open; callbacks do not traverse JSON.
    if (now - h->last_edge < h->phase)
        return;
    h->last_edge = now;
    v3_packet e = {.length = 0,
                   .id = atomic_fetch_add(&h->sequence, 1) + 1,
                   .time = now,
                   .flags = gpio_get_level(__builtin_ctz(h->pins)) ^ h->inverted};
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(h->events, &e, &wake) != pdTRUE)
        atomic_fetch_add(&h->dropped, 1);
    if (wake)
        portYIELD_FROM_ISR();
}
typedef struct { v3_hardware *h; const v3_work *w; } pwm_wait_context;
static uint32_t pwm_read(void *ctx) {
    pwm_wait_context *p = ctx;
    return ledc_get_duty(LEDC_LOW_SPEED_MODE, p->h->channel);
}
static int64_t pwm_now(void *ctx) { (void)ctx; return esp_timer_get_time(); }
static bool pwm_cancelled(void *ctx) { return v3_cancelled(((pwm_wait_context *)ctx)->w); }
static void pwm_wait_tick(void *ctx) { (void)ctx; vTaskDelay(1); }
static int pwm_wait_latched(v3_hardware *h, const v3_work *w, uint32_t target, uint32_t frequency) {
    pwm_wait_context ctx = {.h = h, .w = w};
    v3_pwm_latch_io io = {.ctx = &ctx, .read = pwm_read, .now_us = pwm_now,
                          .cancelled = pwm_cancelled, .wait_tick = pwm_wait_tick};
    v3_pwm_latch_result result = v3_pwm_latch(&io, frequency, target);
    return result == V3_PWM_READY ? ESP_OK : result == V3_PWM_TIMEOUT ? ESP_ERR_TIMEOUT
                                                                           : ESP_ERR_INVALID_STATE;
}
static int simple_open(v3_hardware *h, const v3_work *w) {
    const cJSON *cfg = v3_hw_config(h), *pins = v3_hw_pins(h);
    int pin = v3_int(pins, "pin", -1), err;
    if (h->kind == V_GPIO || h->kind == V_TIMER) {
        h->inverted = v3_bool(cfg, "invert", false);
        bool input = h->kind == V_GPIO && !strcmp(v3_str(cfg, "mode", "input"), "input");
        int level = v3_int(cfg, "level", 0);
        if (h->kind == V_TIMER)
            level = 1;
        gpio_set_level(pin, level ^ h->inverted);
        const char *pull = v3_str(cfg, "pull", "none");
        gpio_config_t g = {.pin_bit_mask = 1ULL << pin,
                           .mode = input ? GPIO_MODE_INPUT
                                   : !strcmp(v3_str(cfg, "mode", "output"), "open_drain")
                                       ? GPIO_MODE_INPUT_OUTPUT_OD
                                       : GPIO_MODE_INPUT_OUTPUT,
                           .pull_up_en = !strcmp(pull, "up"),
                           .pull_down_en = !strcmp(pull, "down"),
                           .intr_type = GPIO_INTR_DISABLE};
        if ((err = gpio_config(&g)))
            return err;
        h->installed = true;
        h->level = level;
        h->start = esp_timer_get_time() / 1000;
        h->next = h->start + v3_int(cfg, "high_ms", 1000);
        h->phase = v3_int(cfg, "debounce_us", 0);
        if ((err = gpio_set_drive_capability(pin, v3_int(cfg, "drive", 2))))
            return err;
        const char *edge = v3_str(cfg, "edge", "none");
        if (strcmp(edge, "none")) {
            if (!isr_installed) {
                err = gpio_install_isr_service(0);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
                    return err;
                isr_installed = true;
            }
            h->events = xQueueCreate(8, sizeof(v3_packet));
            if (!h->events)
                return ESP_ERR_NO_MEM;
            if ((err = gpio_isr_handler_add(pin, gpio_edge, h)))
                return err;
            gpio_int_type_t type = !strcmp(edge, "rising")    ? GPIO_INTR_POSEDGE
                                   : !strcmp(edge, "falling") ? GPIO_INTR_NEGEDGE
                                                              : GPIO_INTR_ANYEDGE;
            if (h->inverted && type != GPIO_INTR_ANYEDGE)
                type = type == GPIO_INTR_POSEDGE ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE;
            if ((err = gpio_set_intr_type(pin, type)))
                return err;
            if ((err = gpio_intr_enable(pin)))
                return err;
            h->enabled = true;
        }
        return ESP_OK;
    }
    if (h->kind == V_PWM) {
        ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                 .timer_num = h->timer,
                                 .duty_resolution = v3_int(cfg, "resolution", 10),
                                 .freq_hz = v3_int(cfg, "frequency", 1000),
                                 .clk_cfg = LEDC_USE_XTAL_CLK};
        uint32_t previous_frequency = pwm_timer_frequency[h->timer];
        if ((err = ledc_timer_config(&t)))
            return err;
        uint32_t frequency = ledc_get_freq(LEDC_LOW_SPEED_MODE, h->timer);
        pwm_timer_frequency[h->timer] = frequency;
        // A frequency/resolution change can still latch on the preceding period.
        if (previous_frequency && previous_frequency < frequency)
            frequency = previous_frequency;
        ledc_channel_config_t ch = {.gpio_num = pin,
                                    .speed_mode = LEDC_LOW_SPEED_MODE,
                                    .channel = h->channel,
                                    .timer_sel = h->timer,
                                    .duty = (uint32_t)((1u << t.duty_resolution) - 1) *
                                            v3_num(cfg, "duty", 50) / 100,
                                    .flags.output_invert = v3_bool(cfg, "invert", false)};
        if ((err = ledc_channel_config(&ch)))
            return err;
        h->installed = true;
        h->start = esp_timer_get_time() / 1000;
        return pwm_wait_latched(h, w, ch.duty, frequency);
    }
    if (h->kind == V_ADC) {
        if (!adc_unit) {
            adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
            if ((err = adc_oneshot_new_unit(&u, &adc_unit)))
                return err;
        }
        ++adc_users;
        h->installed = true;
        adc_oneshot_chan_cfg_t ch = {.atten = v3_int(cfg, "attenuation", 3),
                                     .bitwidth = ADC_BITWIDTH_12};
        if ((err = adc_oneshot_config_channel(adc_unit, pin, &ch)))
            return err;
        adc_cali_curve_fitting_config_t cal = {
            .unit_id = ADC_UNIT_1, .chan = pin, .atten = ch.atten, .bitwidth = ch.bitwidth};
        adc_cali_handle_t handle = NULL;
        err = adc_cali_create_scheme_curve_fitting(&cal, &handle);
        if (err == ESP_OK)
            h->calibration = handle; // Uncalibrated readings are still explicitly reported.
        h->next = esp_timer_get_time() / 1000 + v3_int(cfg, "interval_ms", 0);
        h->events = xQueueCreate(8, sizeof(v3_packet));
        return h->events ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (h->kind == V_TEMPERATURE) {
        temperature_sensor_config_t t = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        temperature_sensor_handle_t sensor = NULL;
        if ((err = temperature_sensor_install(&t, &sensor)))
            return err;
        h->handle = sensor;
        if ((err = temperature_sensor_enable(sensor)))
            return err;
        h->enabled = true;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
static int simple_close(v3_hardware *h) {
    const cJSON *cfg = v3_hw_config(h);
    int err = ESP_OK;
    if (h->kind == V_GPIO && h->enabled) {
        int pin = __builtin_ctz(h->pins);
        if ((err = gpio_intr_disable(pin)))
            return err;
        if ((err = gpio_isr_handler_remove(pin)))
            return err;
        h->enabled = false;
    }
    if (h->kind == V_PWM && h->installed) {
        if (h->fade_active) {
            if ((err = ledc_fade_stop(LEDC_LOW_SPEED_MODE, h->channel)))
                return err;
        }
        if ((err = ledc_stop(LEDC_LOW_SPEED_MODE, h->channel, v3_int(cfg, "end_level", 0))))
            return err;
        h->installed = false;
    }
    if (h->kind == V_ADC && h->installed) {
        if (h->calibration) {
            if ((err = adc_cali_delete_scheme_curve_fitting(h->calibration)))
                return err;
            h->calibration = NULL;
        }
        if (adc_users == 1) {
            if ((err = adc_oneshot_del_unit(adc_unit)))
                return err;
            adc_unit = NULL;
        }
        --adc_users;
        h->installed = false;
    }
    if (h->kind == V_TEMPERATURE && h->handle) {
        if (h->enabled) {
            if ((err = temperature_sensor_disable(h->handle)))
                return err;
            h->enabled = false;
        }
        if ((err = temperature_sensor_uninstall(h->handle)))
            return err;
        h->handle = NULL;
    }
    return ESP_OK;
}
static int adc_read(v3_hardware *h, cJSON **out) {
    int raw, mv = 0, err = adc_oneshot_read(adc_unit, v3_int(v3_hw_pins(h), "pin", 0), &raw);
    if (err)
        return err;
    bool cal = h->calibration && adc_cali_raw_to_voltage(h->calibration, raw, &mv) == ESP_OK;
    *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(*out, "raw", raw);
    cJSON_AddBoolToObject(*out, "calibrated", cal);
    if (cal)
        cJSON_AddNumberToObject(*out, "millivolts", mv);
    else
        cJSON_AddNullToObject(*out, "millivolts");
    cJSON_AddNumberToObject(*out, "sequence", atomic_fetch_add(&h->sequence, 1) + 1);
    cJSON_AddNumberToObject(*out, "time_us", (double)esp_timer_get_time());
    return ESP_OK;
}
cJSON *v3_hw_status(v3_hardware *h) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddBoolToObject(p, "enabled",
                          h->enabled || h->installed || h->tx_enabled || h->rx_enabled);
    v3_add_u32(p, "dropped", atomic_load(&h->dropped));
    v3_add_u32(p, "received", atomic_load(&h->received));
    v3_add_u32(p, "errors", atomic_load(&h->errors));
    v3_add_u32(p, "sequence", atomic_load(&h->sequence));
    cJSON_AddNumberToObject(p, "pending_events", h->events ? uxQueueMessagesWaiting(h->events) : 0);
    if (h->kind == V_GPIO)
        cJSON_AddStringToObject(p, "mode", v3_str(v3_hw_config(h), "mode", "input"));
    if (h->kind == V_GPIO || h->kind == V_TIMER)
        cJSON_AddNumberToObject(p, "level",
                                gpio_get_level(__builtin_ctz(h->pins)) ^
                                    v3_bool(v3_hw_config(h), "invert", false));
    if (h->kind == V_PWM)
        cJSON_AddNumberToObject(p, "duty_percent",
                                ledc_get_duty(LEDC_LOW_SPEED_MODE, h->channel) * 100.0 /
                                    ((1u << v3_int(v3_hw_config(h), "resolution", 10)) - 1));
    return p;
}
static int events_read(v3_hardware *h, cJSON **result) {
    if (!h->events)
        return ESP_ERR_NOT_SUPPORTED;
    *result = v3_hw_status(h);
    cJSON *a = cJSON_AddArrayToObject(*result, "events");
    v3_packet e;
    for (int i = 0; i < 8 && xQueueReceive(h->events, &e, 0) == pdTRUE; ++i) {
        cJSON *p = cJSON_CreateObject();
        v3_add_u32(p, "sequence", e.id);
        v3_add_u32(p, "flags", e.flags);
        cJSON_AddNumberToObject(p, "time_us", (double)e.time);
        char *text = v3_encode64(e.data, e.length);
        if (!text) {
            cJSON_Delete(p);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(p, "data", text);
        free(text);
        v3_push(a, p);
    }
    return ESP_OK;
}
static int simple_call(v3_hardware *h, const v3_work *w, cJSON **result) {
    const cJSON *p = w->params, *cfg = v3_hw_config(h);
    const char *a = w->action;
    int pin = v3_int(v3_hw_pins(h), "pin", -1);
    if (h->kind == V_ADC && !strcmp(a, "read"))
        return adc_read(h, result);
    if (h->kind == V_GPIO) {
        if (!strcmp(a, "read")) {
            *result = v3_hw_status(h);
            return ESP_OK;
        }
        if (!strcmp(a, "write")) {
            if (!strcmp(v3_str(cfg, "mode", "input"), "input") ||
                !v3_integer(v3_get(p, "level"), 0, 1))
                return ESP_ERR_INVALID_ARG;
            int err = gpio_set_level(pin, v3_int(p, "level", 0) ^ v3_bool(cfg, "invert", false));
            if (!err)
                *result = v3_hw_status(h);
            return err;
        }
    }
    if (h->kind == V_PWM && (!strcmp(a, "duty") || !strcmp(a, "fade"))) {
        const cJSON *value = v3_get(p, "duty");
        if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > 100)
            return ESP_ERR_INVALID_ARG;
        uint32_t duty = ((1u << v3_int(cfg, "resolution", 10)) - 1) * value->valuedouble / 100;
        if (h->fade_active) {
            int err = ledc_fade_stop(LEDC_LOW_SPEED_MODE, h->channel);
            if (err)
                return err;
            h->fade_active = false;
        }
        if (!strcmp(a, "fade")) {
            if (v3_int(cfg, "breath_ms", 0))
                return ESP_ERR_INVALID_STATE;
            int err;
            if (!fade_installed) {
                if ((err = ledc_fade_func_install(0)))
                    return err;
                fade_installed = true;
            }
            if ((err = ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, h->channel, duty,
                                               v3_int(p, "duration_ms", 1))))
                return err;
            h->fade_active = true;
            return ledc_fade_start(LEDC_LOW_SPEED_MODE, h->channel, LEDC_FADE_NO_WAIT);
        }
        int err = ledc_set_duty(LEDC_LOW_SPEED_MODE, h->channel, duty);
        if (!err)
            err = ledc_update_duty(LEDC_LOW_SPEED_MODE, h->channel);
        if (!err)
            err = pwm_wait_latched(h, w, duty, ledc_get_freq(LEDC_LOW_SPEED_MODE, h->timer));
        if (!err)
            *result = v3_hw_status(h);
        return err;
    }
    if (h->kind == V_TEMPERATURE && !strcmp(a, "read")) {
        float c;
        int err = temperature_sensor_get_celsius(h->handle, &c);
        if (err)
            return err;
        *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(*result, "celsius", c);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
int v3_peripheral(v3_work *w, cJSON **out, bool *external) {
    if (w->slot < 0 || w->slot >= V3_RESOURCES)
        return ESP_ERR_INVALID_ARG;
    v3_hardware *h = &v3_hardware_slots[w->slot];
    bool simple = w->kind == V_GPIO || w->kind == V_PWM || w->kind == V_ADC || w->kind == V_TIMER ||
                  w->kind == V_TEMPERATURE;
    bool signal = w->kind == V_ADC_CONT || w->kind == V_I2S || w->kind == V_SDM ||
                  w->kind == V_RMT_TX || w->kind == V_RMT_RX;
    if (!strcmp(w->action, "open")) {
        if (v3_cancelled(w))
            return ESP_ERR_INVALID_STATE;
        if (h->id)
            return ESP_ERR_INVALID_STATE;
        atomic_fetch_or(&v3_lane_slots[v3_lane(w->kind, w->controller)], 1u << w->slot);
        memset(h, 0, sizeof(*h));
        h->id = w->resource;
        h->instance = w->instance;
        h->kind = w->kind;
        h->controller = w->controller;
        h->channel = w->channel;
        h->timer = w->timer;
        h->parent_slot = w->parent_slot;
        h->epoch = w->epoch;
        h->definition = cJSON_Duplicate(w->definition, true);
        if (!h->definition)
            return ESP_ERR_NO_MEM;
        const cJSON *pins = v3_hw_pins(h);
        for (const cJSON *v = pins ? pins->child : NULL; v; v = v->next)
            h->pins |= 1u << v->valueint;
        *external = true; // Initialization may already configure hardware before failing.
        int err = power_acquire(h);
        if (err)
            return err;
        err = simple ? simple_open(h, w) : signal ? v3_signal_open(h, w) : v3_bus_open(h, w);
        if (!err && w->kind == V_GPIO)
            *out = v3_hw_status(h);
        return err;
    }
    if (!h->id)
        return !strcmp(w->action, "close") ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (h->id != w->resource || h->instance != w->instance)
        return ESP_ERR_INVALID_STATE;
    if (!strcmp(w->action, "close")) {
        h->suspended = true;
        int err = simple ? simple_close(h) : signal ? v3_signal_close(h) : v3_bus_close(h);
        if (err)
            return err;
        for (int pin = 0; pin < 22; ++pin)
            if (h->pins & (1u << pin)) {
                if ((err = gpio_reset_pin(pin)))
                    return err;
            }
        if (h->events)
            vQueueDelete(h->events);
        if (h->done)
            vSemaphoreDelete(h->done);
        v3_realtime_end(h);
        power_release(h);
        if (!h->shared_memory)
            free(h->memory);
        cJSON_Delete(h->definition);
        atomic_store(&v3_streams[w->slot].instance, 0);
        atomic_fetch_and(&v3_lane_slots[v3_lane(w->kind, w->controller)], ~(1u << w->slot));
        memset(h, 0, sizeof(*h));
        return ESP_OK;
    }
    if (v3_cancelled(w))
        return ESP_ERR_INVALID_STATE;
    if (!strcmp(w->action, "sync"))
        return v3_rmt_sync(w, out, external);
    if (!strcmp(w->action, "group.read") || !strcmp(w->action, "group.write")) {
        const cJSON *slots = v3_get(w->params, "slots"),
                    *resources = v3_get(w->params, "resources");
        int count = cJSON_GetArraySize(slots);
        bool write = !strcmp(w->action, "group.write");
        for (int i = 0; i < count; ++i) {
            int slot = cJSON_GetArrayItem(slots, i)->valueint;
            if (slot < 0 || slot >= V3_RESOURCES || v3_hardware_slots[slot].kind != V_GPIO ||
                v3_hardware_slots[slot].id != (uint32_t)v3_int(cJSON_GetArrayItem(resources, i), "resource", 0) ||
                v3_hardware_slots[slot].instance != (uint32_t)v3_int(cJSON_GetArrayItem(resources, i), "instance", 0))
                return ESP_ERR_INVALID_STATE;
        }
        *out = cJSON_CreateObject();
        cJSON *values = cJSON_AddArrayToObject(*out, "values");
        for (int i = 0; i < count; ++i) {
            if (v3_cancelled(w))
                return ESP_ERR_INVALID_STATE;
            v3_hardware *item = &v3_hardware_slots[cJSON_GetArrayItem(slots, i)->valueint];
            int pin = __builtin_ctz(item->pins),
                invert = v3_bool(v3_hw_config(item), "invert", false);
            if (write) {
                int err = gpio_set_level(pin, v3_int(cJSON_GetArrayItem(resources, i), "level", 0) ^
                                                  invert);
                if (err)
                    return err;
                *external = true;
            }
            cJSON *value = cJSON_CreateObject();
            v3_add_u32(value, "resource", item->id);
            cJSON_AddNumberToObject(value, "level", gpio_get_level(pin) ^ invert);
            v3_push(values, value);
        }
        cJSON_AddNumberToObject(*out, "time_us", (double)esp_timer_get_time());
        return ESP_OK;
    }
    if (!strcmp(w->action, "status")) {
        *out = v3_hw_status(h);
        return ESP_OK;
    }
    if (!strcmp(w->action, "events"))
        return events_read(h, out);
    if ((w->kind == V_GPIO && !strcmp(w->action, "write")) ||
        (w->kind == V_PWM && (!strcmp(w->action, "duty") || !strcmp(w->action, "fade"))))
        *external = true;
    return simple   ? simple_call(h, w, out)
           : signal ? v3_signal_call(h, w, out, external)
                    : v3_bus_call(h, w, out, external);
}
void v3_poll_lane(int lane) {
    int64_t now = esp_timer_get_time() / 1000;
    uint32_t slots = atomic_load(&v3_lane_slots[lane]);
    for (int i = 0; i < V3_RESOURCES; ++i) {
        if (!(slots & (1u << i)))
            continue;
        v3_hardware *h = &v3_hardware_slots[i];
        if (!h->id || h->suspended || h->epoch != atomic_load(&v3_epoch))
            continue;
        v3_bus_poll(h);
        const cJSON *cfg = v3_hw_config(h);
        if (h->kind == V_TIMER && h->installed && now >= h->next) {
            int count = v3_int(cfg, "count", 0), high = v3_int(cfg, "high_ms", 1000),
                low = v3_int(cfg, "low_ms", 1000);
            int64_t cycle = (now - h->start) / (high + low);
            bool done = count && cycle >= count;
            int level = done ? v3_int(cfg, "end_level", 0) : (now - h->start) % (high + low) < high;
            gpio_set_level(__builtin_ctz(h->pins), level);
            h->level = level;
            h->next = now + 1;
            if (done)
                h->installed = false;
        } else if (h->kind == V_PWM && h->installed && v3_int(cfg, "breath_ms", 0)) {
            int duration = v3_int(cfg, "breath_ms", 0);
            double phase = (double)((now - h->start) % duration) / duration;
            double duty =
                v3_num(cfg, "duty", 50) * (0.5 - 0.5 * cos(phase * 2 * 3.141592653589793));
            ledc_set_duty(LEDC_LOW_SPEED_MODE, h->channel,
                          (uint32_t)(((1u << v3_int(cfg, "resolution", 10)) - 1) * duty / 100));
            ledc_update_duty(LEDC_LOW_SPEED_MODE, h->channel);
        } else if (h->kind == V_ADC && h->installed && v3_int(cfg, "interval_ms", 0) &&
                   now >= h->next) {
            cJSON *sample = NULL;
            int err = adc_read(h, &sample);
            h->next = now + v3_int(cfg, "interval_ms", 0);
            if (err)
                atomic_fetch_add(&h->errors, 1);
            else {
                v3_packet e = {
                    .length = 8, .id = atomic_load(&h->sequence), .time = esp_timer_get_time()};
                int32_t raw = v3_int(sample, "raw", 0), mv = v3_int(sample, "millivolts", -1);
                memcpy(e.data, &raw, 4);
                memcpy(e.data + 4, &mv, 4);
                if (xQueueSend(h->events, &e, 0) != pdTRUE)
                    atomic_fetch_add(&h->dropped, 1);
                else
                    atomic_fetch_add(&h->received, 1);
            }
            cJSON_Delete(sample);
        } else if (lane == 7 || lane == 6)
            v3_signal_poll(h, now);
        publish_stream(h);
    }
}
