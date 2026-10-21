#include <math.h>
#include <stdio.h>
#include <string.h>
#include "limivia.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_app_desc.h"

static const int available[] = {0, 1, 3, 4, 5, 6, 7, 10, 13, 20, 21};
typedef enum { IDLE, INPUT, OUTPUT, PWM, ADC, TIMER } pin_mode_t;
static const char *mode_names[] = {"idle", "input", "output", "pwm", "adc", "timer"};
static struct {
    pin_mode_t mode;
    int pull;
} pins[22];
static struct {
    bool used;
    int pin, freq, resolution;
    double duty;
    int breath_ms;
    int64_t start;
} pwms[4];
static struct {
    bool used;
    int pin, high_ms, low_ms, count;
    bool pulse;
    int64_t start;
} timers[4];
static struct {
    bool used;
    int pin, interval_ms;
    int64_t next;
} streams[5];
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static bool calibrated;
static QueueHandle_t requests;
static QueueHandle_t events;
static SemaphoreHandle_t state_lock;
static uint32_t revision;
static char mac_string[18];

static void number(cJSON *o, const char *key, double value) {
    cJSON_AddNumberToObject(o, key, value);
}
static cJSON *object(void) { return cJSON_CreateObject(); }
static bool integer(const cJSON *p, const char *key, int min, int max, int *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < min ||
        v->valuedouble > max || floor(v->valuedouble) != v->valuedouble)
        return false;
    *out = (int)v->valuedouble;
    return true;
}
static bool allowed(int pin) {
    for (size_t i = 0; i < sizeof(available) / sizeof(*available); i++)
        if (available[i] == pin)
            return true;
    return false;
}
static cJSON *error_result(const char *code, const char *message) {
    cJSON *r = object();
    cJSON_AddStringToObject(r, "code", code);
    cJSON_AddStringToObject(r, "message", message);
    return r;
}
static cJSON *snapshot(void) {
    cJSON *s = object();
    number(s, "revision", revision);
    number(s, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddBoolToObject(s, "ble_connected", transport_ble_connected());
    cJSON_AddBoolToObject(s, "usb_active", transport_usb_connected());
    cJSON *a = cJSON_AddArrayToObject(s, "pins");
    for (size_t i = 0; i < sizeof(available) / sizeof(*available); i++) {
        int pin = available[i];
        cJSON *p = object();
        number(p, "pin", pin);
        cJSON_AddStringToObject(p, "mode", mode_names[pins[pin].mode]);
        cJSON_AddStringToObject(p, "pull",
                                pins[pin].pull == 1   ? "up"
                                : pins[pin].pull == 2 ? "down"
                                                      : "none");
        if (pins[pin].mode == INPUT || pins[pin].mode == OUTPUT || pins[pin].mode == TIMER)
            number(p, "level", gpio_get_level(pin));
        else
            cJSON_AddNullToObject(p, "level");
        cJSON_AddItemToArray(a, p);
    }
    a = cJSON_AddArrayToObject(s, "pwm");
    for (int i = 0; i < 4; i++)
        if (pwms[i].used) {
            cJSON *p = object();
            number(p, "pin", pwms[i].pin);
            number(p, "frequency", pwms[i].freq);
            number(p, "duty", pwms[i].duty);
            number(p, "resolution", pwms[i].resolution);
            number(p, "breath_ms", pwms[i].breath_ms);
            cJSON_AddItemToArray(a, p);
        }
    a = cJSON_AddArrayToObject(s, "timers");
    for (int i = 0; i < 4; i++)
        if (timers[i].used) {
            cJSON *p = object();
            number(p, "id", i);
            number(p, "pin", timers[i].pin);
            cJSON_AddStringToObject(p, "kind", timers[i].pulse ? "pulse" : "blink");
            number(p, "high_ms", timers[i].high_ms);
            number(p, "low_ms", timers[i].low_ms);
            number(p, "count", timers[i].count);
            number(p, "elapsed_ms", (esp_timer_get_time() - timers[i].start) / 1000);
            cJSON_AddItemToArray(a, p);
        }
    a = cJSON_AddArrayToObject(s, "streams");
    for (int i = 0; i < 5; i++)
        if (streams[i].used) {
            cJSON *p = object();
            number(p, "pin", streams[i].pin);
            number(p, "interval_ms", streams[i].interval_ms);
            cJSON_AddItemToArray(a, p);
        }
    return s;
}
static esp_err_t configure_gpio(int pin, bool output, int pull) {
    gpio_config_t config = {.pin_bit_mask = 1ULL << pin,
                            .mode = output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
                            .pull_up_en = pull == 1,
                            .pull_down_en = pull == 2,
                            .intr_type = GPIO_INTR_DISABLE};
    return gpio_config(&config);
}
static void release_pin(int pin) {
    for (int i = 0; i < 4; i++) {
        if (pwms[i].used && pwms[i].pin == pin) {
            ledc_stop(LEDC_LOW_SPEED_MODE, i, 0);
            pwms[i].used = false;
        }
        if (timers[i].used && timers[i].pin == pin)
            timers[i].used = false;
    }
    for (int i = 0; i < 5; i++)
        if (streams[i].used && streams[i].pin == pin)
            streams[i].used = false;
    gpio_reset_pin(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    pins[pin].mode = IDLE;
    pins[pin].pull = 0;
}
static cJSON *adc_sample(int pin) {
    int raw;
    if (adc_oneshot_read(adc, pin, &raw) != ESP_OK)
        return NULL;
    cJSON *s = object();
    number(s, "pin", pin);
    number(s, "raw", raw);
    number(s, "uptime_ms", esp_timer_get_time() / 1000);
    int mv;
    bool valid = calibrated && adc_cali_raw_to_voltage(calibration, raw, &mv) == ESP_OK;
    cJSON_AddBoolToObject(s, "calibrated", valid);
    if (valid)
        number(s, "millivolts", mv);
    else
        cJSON_AddNullToObject(s, "millivolts");
    return s;
}
static cJSON *dispatch(const char *op, const cJSON *p, bool *ok, bool *changed) {
    *ok = true;
    *changed = false;
    if (!strcmp(op, "hello")) {
        cJSON *r = object();
        cJSON_AddStringToObject(r, "name", "LIMIVIA");
        cJSON_AddStringToObject(r, "board", "luatos-esp32c3-usb");
        cJSON_AddStringToObject(r, "firmware", esp_app_get_description()->version);
        cJSON_AddStringToObject(r, "device_id", mac_string);
        number(r, "protocol", PROTOCOL_VERSION);
        cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
        const char *features[] = {"ble", "led", "gpio", "pwm", "adc", "timer"};
        cJSON *a = cJSON_AddArrayToObject(caps, "features");
        for (int i = 0; i < 6; i++)
            cJSON_AddItemToArray(a, cJSON_CreateString(features[i]));
        number(caps, "max_pwm", 4);
        number(caps, "max_timers", 4);
        number(caps, "status_led", STATUS_LED);
        number(caps, "demo_led", DEMO_LED);
        number(caps, "adc_max_mv", 2500);
        cJSON_AddBoolToObject(caps, "adc_calibrated", calibrated);
        cJSON_AddItemToObject(r, "state", snapshot());
        return r;
    }
    if (!strcmp(op, "state"))
        return snapshot();
    if (!strcmp(op, "outputs.stop")) {
        for (size_t i = 0; i < sizeof(available) / sizeof(*available); i++)
            release_pin(available[i]);
        *changed = true;
        return snapshot();
    }
    const char *known[] = {"gpio.set",  "gpio.read", "pwm.set",     "pwm.stop",   "adc.read",
                           "adc.start", "adc.stop",  "timer.start", "timer.stop", "pin.release"};
    bool found = false;
    for (size_t i = 0; i < sizeof(known) / sizeof(*known); i++)
        if (!strcmp(op, known[i]))
            found = true;
    if (!found) {
        *ok = false;
        return error_result("unknown_operation", "Unknown operation");
    }
    int pin;
    if (!integer(p, "pin", 0, 21, &pin) || !allowed(pin)) {
        *ok = false;
        return error_result("protected_pin", "Pin unavailable or reserved by board");
    }
    if (!strcmp(op, "pin.release")) {
        release_pin(pin);
        *changed = true;
        return snapshot();
    }
    if (!strcmp(op, "gpio.read")) {
        if (pins[pin].mode != INPUT && pins[pin].mode != OUTPUT && pins[pin].mode != TIMER)
            goto conflict;
        cJSON *r = object();
        number(r, "pin", pin);
        number(r, "level", gpio_get_level(pin));
        return r;
    }
    if (!strcmp(op, "gpio.set")) {
        if (pins[pin].mode != IDLE && pins[pin].mode != INPUT && pins[pin].mode != OUTPUT)
            goto conflict;
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(p, "mode"),
                    *pull = cJSON_GetObjectItemCaseSensitive(p, "pull");
        if (!cJSON_IsString(mode) || !cJSON_IsString(pull))
            goto invalid;
        bool out = !strcmp(mode->valuestring, "output");
        if (!out && strcmp(mode->valuestring, "input"))
            goto invalid;
        int pu = !strcmp(pull->valuestring, "up") ? 1 : !strcmp(pull->valuestring, "down") ? 2 : 0;
        if (!pu && strcmp(pull->valuestring, "none"))
            goto invalid;
        int level = 0;
        if (out && (!integer(p, "level", 0, 1, &level) || pu))
            goto invalid;
        if (out)
            gpio_set_level(pin, level); // preload output latch before enabling output
        if (configure_gpio(pin, out, pu) != ESP_OK)
            goto hardware;
        pins[pin].mode = out ? OUTPUT : INPUT;
        pins[pin].pull = pu;
        *changed = true;
        return snapshot();
    }
    if (!strcmp(op, "pwm.set")) {
        if (pins[pin].mode != IDLE && pins[pin].mode != PWM)
            goto conflict;
        int frequency, breath;
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(p, "duty");
        if (!integer(p, "frequency", 5, 40000, &frequency) ||
            !integer(p, "breath_ms", 0, 60000, &breath) || (breath && breath < 200) ||
            !cJSON_IsNumber(d) || !isfinite(d->valuedouble) || d->valuedouble < 0 ||
            d->valuedouble > 100)
            goto invalid;
        int slot = -1;
        for (int i = 0; i < 4; i++)
            if (pwms[i].used && pwms[i].pin == pin)
                slot = i;
        if (slot < 0)
            for (int i = 0; i < 4; i++)
                if (!pwms[i].used) {
                    slot = i;
                    break;
                }
        if (slot < 0)
            goto capacity;
        int bits = 0;
        // Fixed 40 MHz XTAL clock; divider must fit the C3's 18-bit Q8 divider.
        for (int b = 14; b >= 1; b--) {
            double divider = 40000000.0 / frequency / (1U << b);
            if (divider >= 1 && divider < 1024) {
                bits = b;
                break;
            }
        }
        if (!bits)
            goto invalid;
        ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                     .duty_resolution = bits,
                                     .timer_num = slot,
                                     .freq_hz = frequency,
                                     .clk_cfg = LEDC_USE_XTAL_CLK};
        if (ledc_timer_config(&timer) != ESP_OK)
            goto hardware;
        uint32_t max = (1U << bits) - 1;
        ledc_channel_config_t channel = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = slot,
            .timer_sel = slot,
            .duty = breath ? 0 : (uint32_t)llround(max * d->valuedouble / 100),
            .hpoint = 0};
        if (ledc_channel_config(&channel) != ESP_OK) {
            release_pin(pin);
            goto hardware;
        }
        if (!breath && (d->valuedouble == 0 || d->valuedouble == 100))
            ledc_stop(LEDC_LOW_SPEED_MODE, slot, d->valuedouble == 100);
        pwms[slot].used = true;
        pwms[slot].pin = pin;
        pwms[slot].freq = ledc_get_freq(LEDC_LOW_SPEED_MODE, slot);
        pwms[slot].resolution = bits;
        pwms[slot].duty = d->valuedouble;
        pwms[slot].breath_ms = breath;
        pwms[slot].start = esp_timer_get_time();
        pins[pin].mode = PWM;
        pins[pin].pull = 0;
        *changed = true;
        return snapshot();
    }
    if (!strcmp(op, "pwm.stop")) {
        if (pins[pin].mode != PWM)
            goto conflict;
        release_pin(pin);
        *changed = true;
        return snapshot();
    }
    if (!strcmp(op, "adc.read") || !strcmp(op, "adc.start")) {
        if (pin > 4 || pin == 2)
            goto invalid;
        if (pins[pin].mode != IDLE && pins[pin].mode != ADC)
            goto conflict;
        int interval = 1000;
        bool continuous = !strcmp(op, "adc.start");
        if (continuous && !integer(p, "interval_ms", 100, 10000, &interval))
            goto invalid;
        adc_oneshot_chan_cfg_t cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
        if (adc_oneshot_config_channel(adc, pin, &cfg) != ESP_OK)
            goto hardware;
        pins[pin].mode = ADC;
        if (continuous) {
            streams[pin].used = true;
            streams[pin].pin = pin;
            streams[pin].interval_ms = interval;
            streams[pin].next = esp_timer_get_time() + interval * 1000LL;
        }
        *changed = true;
        cJSON *sample = adc_sample(pin);
        if (!sample)
            goto hardware;
        return sample;
    }
    if (!strcmp(op, "adc.stop")) {
        if (pins[pin].mode != ADC)
            goto conflict;
        release_pin(pin);
        *changed = true;
        return snapshot();
    }
    if (!strcmp(op, "timer.start")) {
        if (pins[pin].mode != IDLE)
            goto conflict;
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(p, "kind");
        int high, low, count;
        if (!cJSON_IsString(kind) || !integer(p, "high_ms", 10, 3600000, &high) ||
            !integer(p, "low_ms", 10, 3600000, &low) || !integer(p, "count", 0, 100000, &count))
            goto invalid;
        bool pulse = !strcmp(kind->valuestring, "pulse");
        if (!pulse && strcmp(kind->valuestring, "blink"))
            goto invalid;
        int slot = -1;
        for (int i = 0; i < 4; i++)
            if (!timers[i].used) {
                slot = i;
                break;
            }
        if (slot < 0)
            goto capacity;
        gpio_set_level(pin, 0);
        if (configure_gpio(pin, true, 0) != ESP_OK)
            goto hardware;
        timers[slot].used = true;
        timers[slot].pin = pin;
        timers[slot].high_ms = high;
        timers[slot].low_ms = low;
        timers[slot].count = count;
        timers[slot].pulse = pulse;
        timers[slot].start = esp_timer_get_time();
        pins[pin].mode = TIMER;
        pins[pin].pull = 0;
        gpio_set_level(pin, 1);
        *changed = true;
        return snapshot();
    }
    if (!strcmp(op, "timer.stop")) {
        if (pins[pin].mode != TIMER)
            goto conflict;
        release_pin(pin);
        *changed = true;
        return snapshot();
    }
invalid:
    *ok = false;
    return error_result("invalid_argument", "Invalid parameter or unsupported range");
conflict:
    *ok = false;
    return error_result("pin_busy", "Release the pin before changing its function");
capacity:
    *ok = false;
    return error_result("capacity", "All hardware resources are occupied");
hardware:
    *ok = false;
    return error_result("hardware_error", "Driver operation failed; refresh device state");
}
static void publish(cJSON *event) {
    if (xQueueSend(events, &event, 0) != pdTRUE)
        cJSON_Delete(event);
}
static void event_worker(void *arg) {
    cJSON *event;
    for (;;)
        if (xQueueReceive(events, &event, portMAX_DELAY) == pdTRUE) {
            transport_broadcast(event);
            cJSON_Delete(event);
        }
}
static void changed_event(void) {
    cJSON *e = object();
    number(e, "v", 1);
    cJSON_AddStringToObject(e, "event", "changed");
    number(e, "revision", revision);
    publish(e);
}
static void command_worker(void *arg) {
    request_t request;
    for (;;) {
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE)
            continue;
        const char *end = NULL;
        cJSON *root = cJSON_ParseWithOpts(request.text, &end, true);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id"),
              *op = cJSON_GetObjectItemCaseSensitive(root, "op");
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        int version = 0;
        bool valid = cJSON_IsObject(root) && integer(root, "v", 1, 1, &version) &&
                     cJSON_IsString(op) && cJSON_IsNumber(id) && id->valuedouble >= 1 &&
                     id->valuedouble <= 2147483647 && floor(id->valuedouble) == id->valuedouble &&
                     cJSON_IsObject(params);
        bool ok = false, changed = false;
        cJSON *result;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        if (valid)
            result = dispatch(op->valuestring, params, &ok, &changed);
        else
            result = error_result("invalid_request",
                                  "Expected protocol v1, integer id, op and params object");
        if (changed) {
            revision++;
            cJSON *rev = cJSON_GetObjectItemCaseSensitive(result, "revision");
            if (rev)
                cJSON_SetNumberValue(rev, revision);
        }
        xSemaphoreGive(state_lock);
        cJSON *response = object();
        number(response, "v", 1);
        if (cJSON_IsNumber(id))
            number(response, "id", id->valuedouble);
        else
            cJSON_AddNullToObject(response, "id");
        cJSON_AddBoolToObject(response, "ok", ok);
        cJSON_AddItemToObject(response, ok ? "result" : "error", result);
        transport_send(request.source, request.generation, response);
        cJSON_Delete(response);
        cJSON_Delete(root);
        if (changed)
            changed_event();
    }
}
static void tick_worker(void *arg) {
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(10));
        int64_t now = esp_timer_get_time();
        bool changed = false;
        cJSON *samples[5] = {0};
        xSemaphoreTake(state_lock, portMAX_DELAY);
        gpio_set_level(STATUS_LED, transport_ble_connected() || transport_usb_connected());
        for (int i = 0; i < 4; i++) {
            if (pwms[i].used && pwms[i].breath_ms) {
                double phase =
                    ((now - pwms[i].start) / 1000 % pwms[i].breath_ms) / (double)pwms[i].breath_ms;
                double scale = phase < 0.5 ? phase * 2 : (1 - phase) * 2;
                uint32_t duty =
                    llround(((1U << pwms[i].resolution) - 1) * pwms[i].duty / 100 * scale);
                ledc_set_duty(LEDC_LOW_SPEED_MODE, i, duty);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, i);
            }
            if (timers[i].used) {
                int64_t elapsed = (now - timers[i].start) / 1000;
                int64_t period = (int64_t)timers[i].high_ms + timers[i].low_ms;
                bool done = timers[i].pulse
                                ? elapsed >= timers[i].high_ms
                                : timers[i].count > 0 && elapsed >= period * timers[i].count;
                if (done) {
                    gpio_set_level(timers[i].pin, 0);
                    pins[timers[i].pin].mode = OUTPUT;
                    timers[i].used = false;
                    changed = true;
                } else
                    gpio_set_level(timers[i].pin, elapsed % period < timers[i].high_ms);
            }
        }
        for (int i = 0; i < 5; i++)
            if (streams[i].used && now >= streams[i].next) {
                streams[i].next = now + streams[i].interval_ms * 1000LL;
                samples[i] = adc_sample(streams[i].pin);
            }
        if (changed)
            revision++;
        xSemaphoreGive(state_lock);
        if (changed)
            changed_event();
        for (int i = 0; i < 5; i++)
            if (samples[i]) {
                cJSON *event = object();
                number(event, "v", 1);
                cJSON_AddStringToObject(event, "event", "adc");
                cJSON_AddItemToObject(event, "data", samples[i]);
                publish(event);
            }
    }
}
void control_submit(const request_t *request) { xQueueSend(requests, request, 0); }
void control_init(void) {
    state_lock = xSemaphoreCreateMutex();
    requests = xQueueCreate(8, sizeof(request_t));
    events = xQueueCreate(16, sizeof(cJSON *));
    configASSERT(state_lock && requests && events);
    for (size_t i = 0; i < sizeof(available) / sizeof(*available); i++)
        release_pin(available[i]);
    gpio_set_direction(STATUS_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(STATUS_LED, 0);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    adc_oneshot_unit_init_cfg_t config = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&config, &adc));
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    calibrated = adc_cali_create_scheme_curve_fitting(&cal, &calibration) == ESP_OK;
    configASSERT(xTaskCreate(command_worker, "commands", 8192, NULL, 5, NULL) == pdPASS);
    configASSERT(xTaskCreate(event_worker, "events", 4096, NULL, 4, NULL) == pdPASS);
    configASSERT(xTaskCreate(tick_worker, "local-tasks", 4096, NULL, 6, NULL) == pdPASS);
}
