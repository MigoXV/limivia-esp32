#include "v3.h"
#include <math.h>
#include <string.h>

const char *const v3_kinds[V_KIND_COUNT] = {
    "gpio",       "pwm",       "adc",        "adc.continuous", "uart",      "i2c.master",
    "i2c.device", "i2c.slave", "spi.master", "spi.device",     "spi.slave", "twai",
    "rmt.tx",     "rmt.rx",    "i2s",        "sigma_delta",    "timer",     "temperature"};
const char *const v3_states[] = {"free", "opening", "ready", "closing", "fault"};
const char *v3_pin_reserved(int pin) {
    switch (pin) {
    case 2:
    case 8:
    case 9:
        return "boot_strap";
    case 11:
        return "flash_voltage";
    case 12:
        return "status_led";
    case 14:
    case 15:
    case 16:
    case 17:
        return "flash";
    case 18:
    case 19:
        return "native_usb";
    default:
        return pin < 0 || pin > 21 ? "not_exposed" : NULL;
    }
}
bool v3_pin_allowed(int pin) { return !v3_pin_reserved(pin); }
bool v3_available(const char *profile, v3_kind kind) {
    if (kind < 0 || kind >= V_KIND_COUNT)
        return false;
    if (kind == V_ADC_CONT || kind == V_I2S || kind == V_SDM)
        return !strcmp(profile, "signal");
    if (kind == V_I2C_SLAVE || kind == V_SPI_SLAVE)
        return !strcmp(profile, "bus");
    if (kind == V_UART || kind == V_SPI || kind == V_SPI_DEVICE || kind == V_TWAI)
        return strcmp(profile, "signal") != 0;
    return true;
}
v3_resource *v3_find(v3_core *c, uint32_t id) {
    if (!id)
        return NULL;
    for (int i = 0; i < V3_RESOURCES; ++i)
        if (c->resources[i].state != V_FREE && c->resources[i].id == id)
            return &c->resources[i];
    return NULL;
}
// These descriptors are the authority for public configuration and discovery.
typedef struct {
    const char *key;
    char type;
    double low, high;
    const char *values;
} field;
#define N(k, a, b) {k, 'n', a, b, NULL}
#define F(k, a, b) {k, 'f', a, b, NULL}
#define B(k) {k, 'b', 0, 0, NULL}
#define E(k, v) {k, 's', 0, 0, v}
static const field gpio_f[] = {E("mode", "input output open_drain"),
                               E("pull", "none up down"),
                               N("level", 0, 1),
                               N("drive", 0, 3),
                               B("invert"),
                               E("edge", "none rising falling any"),
                               N("debounce_us", 0, 1000000)};
static const field pwm_f[] = {N("frequency", 1, 40000000), N("resolution", 1, 14),
                              F("duty", 0, 100),           B("invert"),
                              N("end_level", 0, 1),        N("breath_ms", 0, 3600000)};
static const field adc_f[] = {N("attenuation", 0, 3), N("interval_ms", 0, 3600000)};
static const field adc_cont_f[] = {N("attenuation", 0, 3), N("sample_rate", 611, 83333),
                                   N("pool_bytes", 1024, 8192)};
static const field uart_f[] = {N("baud", 50, 5000000),
                               N("data_bits", 5, 8),
                               E("parity", "none even odd"),
                               N("stop_bits", 1, 2),
                               B("flow_control"),
                               N("rx_threshold", 1, 120),
                               N("invert_mask", 0, 255),
                               E("mode", "uart rs485"),
                               N("rx_buffer", 256, 4096),
                               N("tx_buffer", 256, 4096),
                               B("loopback")};
static const field i2c_f[] = {B("pullup"), N("glitch_ignore", 0, 7)};
static const field i2c_dev_f[] = {N("address", 0, 1023), N("address_bits", 7, 10),
                                  N("frequency", 1000, 400000), N("scl_wait_us", 0, 1000000)};
static const field i2c_slave_f[] = {N("address", 0, 1023), N("address_bits", 7, 10), B("pullup"),
                                    N("buffer_bytes", 128, 4096)};
static const field spi_f[] = {N("max_transfer", 4, 4096), B("quad")};
static const field spi_dev_f[] = {N("frequency", 1000, 80000000),
                                  N("mode", 0, 3),
                                  N("command_bits", 0, 16),
                                  N("address_bits", 0, 64),
                                  N("dummy_bits", 0, 256),
                                  B("lsb_first"),
                                  B("half_duplex"),
                                  N("queue_depth", 1, 4)};
static const field spi_slave_f[] = {N("mode", 0, 3), B("lsb_first"), N("max_transfer", 4, 4096),
                                    N("queue_depth", 1, 4)};
static const field twai_f[] = {
    N("bitrate", 1000, 1000000),    E("mode", "normal listen_only self_test"),
    N("queue_depth", 1, 16),        N("filter_id", 0, 536870911),
    N("filter_mask", 0, 536870911), B("extended")};
static const field rmt_f[] = {N("resolution_hz", 100000, 40000000), B("invert"),
                              N("carrier_hz", 0, 10000000), F("carrier_duty", 0, 1),
                              N("idle_level", 0, 1)};
static const field i2s_f[] = {E("mode", "standard tdm pdm"),
                              E("role", "master slave"),
                              E("direction", "tx rx duplex"),
                              N("sample_rate", 1000, 6144000),
                              N("bits", 8, 32),
                              N("slots", 1, 16),
                              N("slot_mask", 1, 65535),
                              N("dma_frames", 16, 512),
                              B("left_align"),
                              B("bit_shift")};
static const field sdm_f[] = {N("sample_rate", 1000, 20000000), N("density", -128, 127),
                              B("invert")};
static const field timer_f[] = {N("high_ms", 10, 3600000), N("low_ms", 10, 3600000),
                                N("count", 0, 100000), N("end_level", 0, 1)};
typedef struct {
    const field *fields;
    size_t count;
    const char *pins;
    const char *required;
    int family, capacity;
} definition;
#define D(f, p, r, a, b) {f, sizeof(f) / sizeof(*f), p, r, a, b}
static const definition definitions[V_KIND_COUNT] = {
    D(gpio_f, "pin", "pin", 0, 11),
    D(pwm_f, "pin", "pin", 1, 6),
    D(adc_f, "pin", "pin", 2, 4),
    D(adc_cont_f, "ch0 ch1 ch3 ch4", "", 2, 1),
    D(uart_f, "tx rx rts cts", "", 3, 2),
    D(i2c_f, "sda scl", "sda scl", 4, 1),
    D(i2c_dev_f, "", "", -1, 8),
    D(i2c_slave_f, "sda scl", "sda scl", 4, 1),
    D(spi_f, "sclk mosi miso wp hd", "sclk", 5, 1),
    D(spi_dev_f, "cs", "cs", -1, 6),
    D(spi_slave_f, "sclk mosi miso cs ready", "sclk cs", 5, 1),
    D(twai_f, "tx rx", "tx rx", 6, 1),
    D(rmt_f, "pin", "pin", 7, 2),
    D(rmt_f, "pin", "pin", 8, 2),
    D(i2s_f, "bclk ws din dout mclk", "bclk", 9, 1),
    D(sdm_f, "pin", "pin", 10, 4),
    D(timer_f, "pin", "pin", 11, 4),
    {NULL, 0, "", "", 12, 1}};
static bool word(const char *words, const char *s) {
    for (const char *p = words; *p;) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (strlen(s) == n && !memcmp(p, s, n))
            return true;
        p += n;
        if (*p)
            ++p;
    }
    return false;
}
static bool config_valid(v3_kind kind, const cJSON *cfg) {
    if (!cJSON_IsObject(cfg))
        return false;
    const definition *d = &definitions[kind];
    for (const cJSON *v = cfg->child; v; v = v->next) {
        const field *f = NULL;
        for (size_t i = 0; i < d->count; ++i)
            if (!strcmp(d->fields[i].key, v->string))
                f = &d->fields[i];
        if (!f)
            return false;
        if (f->type == 'n' && !v3_integer(v, f->low, f->high))
            return false;
        if (f->type == 'f' && (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
                               v->valuedouble < f->low || v->valuedouble > f->high))
            return false;
        if (f->type == 'b' && !cJSON_IsBool(v))
            return false;
        if (f->type == 's' && (!cJSON_IsString(v) || !word(f->values, v->valuestring)))
            return false;
    }
    if ((kind == V_I2C_DEVICE || kind == V_I2C_SLAVE) &&
        (!v3_integer(v3_get(cfg, "address"), 0,
                     v3_int(cfg, "address_bits", 7) == 10 ? 1023 : 127) ||
         (v3_int(cfg, "address_bits", 7) != 7 && v3_int(cfg, "address_bits", 7) != 10)))
        return false;
    if (kind == V_I2S) {
        int bits = v3_int(cfg, "bits", 16);
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
            return false;
        if (!strcmp(v3_str(cfg, "mode", "standard"), "pdm") && bits != 16)
            return false;
    }
    if (kind == V_PWM) {
        int hz = v3_int(cfg, "frequency", 1000), bits = v3_int(cfg, "resolution", 10);
        if ((double)hz * (1u << bits) > 40000000)
            return false;
    }
    return true;
}
const char *v3_plan(v3_core *c, const cJSON *p, v3_resource *out) {
    memset(out, 0, sizeof(*out));
    out->controller = out->channel = out->timer = -1;
    if (!v3_keys(p, "kind parent parent_instance controller pins config"))
        return "invalid_argument";
    const char *kind = v3_str(p, "kind", "");
    int k = -1;
    for (int i = 0; i < V_KIND_COUNT; ++i)
        if (!strcmp(kind, v3_kinds[i]))
            k = i;
    if (k < 0)
        return "unknown_resource_kind";
    out->kind = k;
    if (!v3_available(c->profile, k))
        return "feature_unavailable";
    const definition *d = &definitions[k];
    const cJSON *pins = v3_get(p, "pins"), *cfg = v3_get(p, "config");
    if (!v3_keys(pins, d->pins) || !config_valid(k, cfg))
        return "invalid_argument";
    for (const char *r = d->required; *r;) {
        const char *e = strchr(r, ' ');
        size_t n = e ? (size_t)(e - r) : strlen(r);
        char name[16] = {0};
        memcpy(name, r, n);
        if (!v3_get(pins, name))
            return "missing_pin";
        r += n;
        if (*r)
            ++r;
    }
    for (const cJSON *v = pins->child; v; v = v->next) {
        if (!v3_integer(v, 0, 21))
            return "invalid_pin";
        int pin = v->valueint;
        if (!v3_pin_allowed(pin))
            return "reserved_pin";
        if ((out->pins & (1u << pin)) && !(k == V_UART && v3_bool(cfg, "loopback", false)))
            return "duplicate_pin";
        out->pins |= 1u << pin;
        if (k == V_ADC && pin != 0 && pin != 1 && pin != 3 && pin != 4)
            return "adc_unavailable";
        if (k == V_ADC_CONT) {
            const char *role = v->string;
            int expected = !strcmp(role, "ch0")   ? 0
                           : !strcmp(role, "ch1") ? 1
                           : !strcmp(role, "ch3") ? 3
                                                  : 4;
            if (pin != expected)
                return "fixed_adc_route";
        }
    }
    if ((k == V_UART || k == V_ADC_CONT) && !out->pins)
        return "missing_pin";
    if (k == V_UART && v3_bool(cfg, "flow_control", false) &&
        (!v3_get(pins, "rts") || !v3_get(pins, "cts")))
        return "missing_pin";
    if (k == V_UART && !strcmp(v3_str(cfg, "mode", "uart"), "rs485") &&
        (!v3_get(pins, "tx") || !v3_get(pins, "rx") || !v3_get(pins, "rts") ||
         v3_bool(cfg, "flow_control", false)))
        return "unsupported_configuration";
    if (k == V_I2S) {
        const char *dir = v3_str(cfg, "direction", "duplex"),
                   *mode = v3_str(cfg, "mode", "standard");
        if (strcmp(dir, "rx") && !v3_get(pins, "dout"))
            return "missing_pin";
        if (strcmp(dir, "tx") && !v3_get(pins, "din"))
            return "missing_pin";
        if (strcmp(mode, "pdm") && !v3_get(pins, "ws"))
            return "missing_pin";
        if (!strcmp(mode, "pdm") && !strcmp(dir, "duplex"))
            return "unsupported_configuration";
    }
    if (v3_get(p, "parent") && !v3_integer(v3_get(p, "parent"), 1, 2147483647))
        return "invalid_argument";
    out->parent = v3_int(p, "parent", 0);
    v3_resource *parent = v3_find(c, out->parent);
    if (k == V_I2C_DEVICE || k == V_SPI_DEVICE) {
        if (!parent || parent->instance != (uint32_t)v3_int(p, "parent_instance", 0) ||
            parent->state != V_READY ||
            parent->kind != (k == V_I2C_DEVICE ? V_I2C : V_SPI))
            return "invalid_parent";
        if (parent->busy || parent->flow)
            return "resource_busy";
        out->controller = parent->controller;
    } else if (out->parent)
        return "invalid_parent";
    for (int i = 0; i < V3_RESOURCES; ++i) {
        const v3_resource *r = &c->resources[i];
        if (r->state == V_FREE)
            continue;
        if (r->pins & out->pins)
            return "pin_conflict";
        if ((k == V_ADC_CONT && (r->kind == V_ADC || r->kind == V_ADC_CONT)) ||
            (k == V_ADC && r->kind == V_ADC_CONT))
            return "adc_unit_conflict";
        if (k == V_I2C_DEVICE && r->kind == V_I2C_DEVICE && r->parent == out->parent &&
            v3_int(v3_get(r->definition, "config"), "address", -1) == v3_int(cfg, "address", -2))
            return "duplicate_device";
    }
    if (d->family >= 0) {
        if (v3_get(p, "controller") && !v3_integer(v3_get(p, "controller"), 0, k == V_UART ? 1 : 0))
            return "invalid_controller";
        int requested = v3_int(p, "controller", -1), limit = k == V_UART ? 2 : 1;
        bool allocated = false;
        for (int controller = 0; controller < limit && !allocated; ++controller) {
            if (requested >= 0 && controller != requested)
                continue;
            for (int channel = 0; channel < d->capacity && !allocated; ++channel) {
                bool busy = false;
                for (int i = 0; i < V3_RESOURCES; ++i) {
                    const v3_resource *r = &c->resources[i];
                    if (r->state != V_FREE && definitions[r->kind].family == d->family &&
                        r->controller == controller && (k == V_UART || r->channel == channel))
                        busy = true;
                }
                if (!busy) {
                    out->controller = controller;
                    out->channel = channel;
                    allocated = true;
                }
            }
        }
        if (!allocated)
            return "controller_conflict";
    } else {
        int count = 0;
        for (int i = 0; i < V3_RESOURCES; ++i)
            if (c->resources[i].state != V_FREE && c->resources[i].parent == out->parent)
                ++count;
        if (count >= d->capacity)
            return "device_capacity";
    }
    if (k == V_PWM) {
        int free_timer = -1;
        for (int t = 0; t < 4; ++t) {
            bool used = false, compatible = true;
            for (int i = 0; i < V3_RESOURCES; ++i) {
                const v3_resource *r = &c->resources[i];
                if (r->state != V_FREE && r->kind == V_PWM && r->timer == t) {
                    used = true;
                    const cJSON *other = v3_get(r->definition, "config");
                    if (v3_int(other, "frequency", 1000) != v3_int(cfg, "frequency", 1000) ||
                        v3_int(other, "resolution", 10) != v3_int(cfg, "resolution", 10))
                        compatible = false;
                }
            }
            if (used && compatible) {
                out->timer = t;
                break;
            }
            if (!used)
                free_timer = t;
        }
        if (out->timer < 0)
            out->timer = free_timer;
        if (out->timer < 0)
            return "pwm_timer_conflict";
    }
    return NULL;
}
cJSON *v3_resource_json(const v3_resource *r) {
    cJSON *p = cJSON_CreateObject();
    v3_add_u32(p, "id", r->id);
    v3_add_u32(p, "instance", r->instance);
    cJSON_AddStringToObject(p, "kind", v3_kinds[r->kind]);
    cJSON_AddStringToObject(p, "state", v3_states[r->state]);
    v3_add_u32(p, "parent", r->parent);
    v3_add_u32(p, "pin_mask", r->pins);
    v3_add_u32(p, "job_id", r->busy);
    cJSON_AddNumberToObject(p, "controller", r->controller);
    cJSON_AddNumberToObject(p, "channel", r->channel);
    cJSON_AddNumberToObject(p, "timer", r->timer);
    cJSON_AddNumberToObject(p, "driver_error", r->error);
    cJSON_AddNumberToObject(p, "cleanup_error", r->cleanup_error);
    v3_add_u32(p, "flow", r->flow);
    v3_put(p, "definition", cJSON_Duplicate(r->definition, true));
    return p;
}
cJSON *v3_capabilities(const v3_core *c, int page) {
    cJSON *p = cJSON_CreateObject(), *items = cJSON_AddArrayToObject(p, "items");
    cJSON_AddNumberToObject(p, "page", page);
    cJSON_AddNumberToObject(p, "pages", v3_capability_pages());
    if (page >= 21) {
        v3_put(p, "operations", v3_operations_json(c->profile, page - 21));
    } else if (page >= 3 && page < 21) {
        v3_kind kind = page - 3;
        cJSON_AddStringToObject(p, "kind", v3_kinds[kind]);
        cJSON_AddBoolToObject(p, "available", v3_available(c->profile, kind));
        v3_push(items, v3_actions_json(kind));
    } else if (page == 0) {
        for (int i = 0; i < 22; ++i) {
            cJSON *pin = cJSON_CreateObject();
            cJSON_AddNumberToObject(pin, "pin", i);
            const char *reason = v3_pin_reserved(i);
            cJSON_AddBoolToObject(pin, "digital", !reason);
            cJSON_AddBoolToObject(pin, "adc1", i == 0 || i == 1 || i == 3 || i == 4);
            if (i <= 4)
                cJSON_AddNumberToObject(pin, "chip_adc1_channel", i);
            cJSON_AddBoolToObject(pin, "deep_sleep_wake", !reason && i <= 5);
            if (reason)
                cJSON_AddStringToObject(pin, "reserved", reason);
            if (i == 13)
                cJSON_AddStringToObject(pin, "load", "onboard_led_D5");
            if (i == 5)
                cJSON_AddStringToObject(pin, "analog_unavailable", "adc2_chip_limitation");
            v3_push(items, pin);
        }
    } else if (page == 1 || page == 2) {
        int start = page == 1 ? 0 : 9, end = page == 1 ? 9 : V_KIND_COUNT;
        for (int i = start; i < end; ++i) {
            const definition *d = &definitions[i];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "kind", v3_kinds[i]);
            cJSON_AddBoolToObject(item, "available", v3_available(c->profile, i));
            cJSON_AddNumberToObject(item, "capacity", d->capacity);
            cJSON_AddStringToObject(item, "pin_roles", d->pins);
            cJSON_AddStringToObject(item, "required_pins", d->required);
            cJSON *fields = cJSON_AddArrayToObject(item, "config");
            for (size_t j = 0; j < d->count; ++j) {
                const field *f = &d->fields[j];
                cJSON *v = cJSON_CreateObject();
                cJSON_AddStringToObject(v, "name", f->key);
                cJSON_AddStringToObject(v, "type",
                                        f->type == 's'   ? "enum"
                                        : f->type == 'b' ? "boolean"
                                        : f->type == 'n' ? "integer"
                                                         : "number");
                if (f->type == 's')
                    cJSON_AddStringToObject(v, "values", f->values);
                else if (f->type == 'n' || f->type == 'f') {
                    cJSON_AddNumberToObject(v, "min", f->low);
                    cJSON_AddNumberToObject(v, "max", f->high);
                }
                v3_push(fields, v);
            }
            v3_push(items, item);
        }
    }
    return p;
}
