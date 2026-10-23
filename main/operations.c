#include "v3.h"
#include <math.h>
#include <string.h>

typedef struct {
    const char *name;
    char type;
    bool required;
    double min, max;
} argument;
#define I(k, r, a, b) {k, 'i', r, a, b}
#define N(k, r, a, b) {k, 'n', r, a, b}
#define S(k, r) {k, 's', r, 0, 0}
#define B(k, r) {k, 'b', r, 0, 0}
static const argument level[] = {I("level", true, 0, 1)};
static const argument duty[] = {N("duty", true, 0, 100)};
static const argument fade[] = {N("duty", true, 0, 100), I("duration_ms", true, 1, 3600000)};
static const argument read_args[] = {I("length", true, 1, V3_BUFFER_MAX)};
static const argument write_args[] = {S("data", false), I("buffer_id", false, 1, 2147483647)};
static const argument break_args[] = {S("data", true), I("bits", true, 1, 255)};
static const argument probe[] = {I("address", true, 0, 127)};
static const argument write_read[] = {S("data", false), I("buffer_id", false, 1, 2147483647),
                                      I("length", true, 1, V3_BUFFER_MAX)};
static const argument spi[] = {S("data", false),
                               I("buffer_id", false, 1, 2147483647),
                               I("rx_length", false, 0, V3_BUFFER_MAX),
                               I("command", false, 0, 65535),
                               S("address_hex", false),
                               I("lines", false, 1, 4)};
static const argument can[] = {S("data", true), I("id", true, 0, 536870911), B("extended", false),
                               B("remote", false)};
static const argument pulses[] = {S("data", false), I("buffer_id", false, 1, 2147483647),
                                  I("loops", false, -1, 1023)};
static const argument capture[] = {I("min_ns", false, 0, 4294967295.0),
                                   I("max_ns", true, 1, 4294967295.0)};
static const argument density[] = {I("density", true, -128, 127)};
typedef struct {
    v3_kind kind;
    const char *name;
    const argument *args;
    size_t count;
    bool payload;
} action;
#define A(k, n, a, p) {k, n, a, sizeof(a) / sizeof(*a), p}
#define Z(k, n) {k, n, NULL, 0, false}
static const action actions[] = {Z(V_GPIO, "read"),
                                 A(V_GPIO, "write", level, false),
                                 A(V_PWM, "duty", duty, false),
                                 A(V_PWM, "fade", fade, false),
                                 Z(V_ADC, "read"),
                                 Z(V_ADC_CONT, "start"),
                                 Z(V_ADC_CONT, "stop"),
                                 Z(V_ADC_CONT, "read"),
                                 A(V_UART, "write", write_args, true),
                                 A(V_UART, "read", read_args, false),
                                 A(V_UART, "break", break_args, true),
                                 Z(V_UART, "drain"),
                                 Z(V_UART, "clear"),
                                 Z(V_UART, "errors"),
                                 A(V_I2C, "probe", probe, false),
                                 Z(V_I2C, "scan"),
                                 Z(V_I2C, "recover"),
                                 A(V_I2C_DEVICE, "write", write_args, true),
                                 A(V_I2C_DEVICE, "read", read_args, false),
                                 A(V_I2C_DEVICE, "write_read", write_read, true),
                                 A(V_I2C_SLAVE, "preload", write_args, true),
                                 Z(V_I2C_SLAVE, "clear_tx"),
                                 A(V_SPI_DEVICE, "transfer", spi, false),
                                 Z(V_SPI_DEVICE, "result"),
                                 A(V_SPI_SLAVE, "queue", spi, false),
                                 Z(V_SPI_SLAVE, "result"),
                                 Z(V_SPI_SLAVE, "cancel"),
                                 A(V_TWAI, "send", can, true),
                                 Z(V_TWAI, "info"),
                                 Z(V_TWAI, "recover"),
                                 A(V_RMT_TX, "transmit", pulses, true),
                                 Z(V_RMT_TX, "abort"),
                                 Z(V_RMT_TX, "wait"),
                                 A(V_RMT_RX, "capture", capture, false),
                                 Z(V_RMT_RX, "result"),
                                 Z(V_RMT_RX, "abort"),
                                 Z(V_I2S, "start"),
                                 Z(V_I2S, "stop"),
                                 A(V_I2S, "play", write_args, true),
                                 A(V_I2S, "record", read_args, false),
                                 A(V_SDM, "density", density, false),
                                 Z(V_TEMPERATURE, "read")};
static const action *find(v3_kind k, const char *name) {
    for (size_t i = 0; i < sizeof(actions) / sizeof(*actions); ++i)
        if (actions[i].kind == k && !strcmp(actions[i].name, name))
            return &actions[i];
    return NULL;
}
typedef struct {
    const char *name, *keys;
} operation;
static const operation operations[] = {
    {"hello", ""},
    {"session.ping", ""},
    {"event.subscribe", "topics interval_ms"},
    {"event.unsubscribe", ""},
    {"control.acquire", ""},
    {"control.release", ""},
    {"outputs.stop", ""},
    {"capabilities", "page"},
    {"state", "page revision"},
    {"diagnostics", ""},
    {"mux.query", "page revision"},
    {"mux.preflight", "kind parent controller pins config"},
    {"resource.preflight", "kind parent controller pins config"},
    {"resource.open", "kind parent controller pins config"},
    {"resource.get", "resource"},
    {"resource.close", "resource cascade"},
    {"resource.recover", "resource cascade"},
    {"resource.call", "resource action args"},
    {"job.get", "job_id"},
    {"job.cancel", "job_id"},
    {"gpio.group.read", "resources"},
    {"gpio.group.write", "resources"},
    {"rmt.sync", "resources timeout_ms"},
    {"buffer.create", "size"},
    {"buffer.write", "buffer_id offset data"},
    {"buffer.read", "buffer_id offset length"},
    {"buffer.delete", "buffer_id"},
    {"storage.status", ""},
    {"storage.list", "page"},
    {"storage.format", "confirm"},
    {"storage.begin", "name size sha256 secret"},
    {"storage.chunk", "upload_id offset data"},
    {"storage.commit", "upload_id"},
    {"storage.abort", "upload_id"},
    {"storage.read", "name offset length"},
    {"storage.delete", "name secret"},
    {"storage.record", "name value"},
    {"storage.append", "name value limit_bytes"},
    {"ota.status", ""},
    {"ota.begin", "board protocol profile size sha256"},
    {"ota.chunk", "upload_id offset data"},
    {"ota.commit", "upload_id reboot"},
    {"ota.abort", "upload_id"},
    {"wifi.status", ""},
    {"wifi.configure", "ssid password persist"},
    {"wifi.scan", ""},
    {"wifi.maintenance", "enable"},
    {"wifi.disconnect", ""},
    {"mqtt.status", ""},
    {"mqtt.configure", "uri protocol username password ca_pem ca_file certificate_file "
                       "private_key_file will_topic will will_qos will_retain persist"},
    {"mqtt.publish", "topic data buffer_id json qos retain"},
    {"mqtt.subscribe", "topic qos"},
    {"mqtt.unsubscribe", "topic"},
    {"mqtt.receive", ""},
    {"mqtt.stop", ""},
    {"espnow.status", ""},
    {"espnow.start", "pmk"},
    {"espnow.stop", ""},
    {"espnow.peer", "mac encrypt key"},
    {"espnow.remove", "mac"},
    {"espnow.send", "mac data buffer_id timeout_ms"},
    {"espnow.receive", ""},
    {"power.configure", "auto_light_sleep modem_sleep"},
    {"power.sleep", "mode duration_ms wake_pin wake_level"},
    {"system.info", ""},
    {"system.time", ""},
    {"system.restart", ""},
    {"system.nvs_format", "confirm"},
    {"flow.load", "name"},
    {"flow.list", ""},
    {"flow.get", "flow_id"},
    {"flow.start", "flow_id"},
    {"flow.pause", "flow_id"},
    {"flow.resume", "flow_id"},
    {"flow.stop", "flow_id"},
    {"flow.remove", "flow_id"}};
int v3_capability_pages(void) { return 21 + (sizeof(operations) / sizeof(*operations) + 11) / 12; }
cJSON *v3_operations_json(const char *profile, int page) {
    cJSON *a = cJSON_CreateArray();
    size_t start = (size_t)page * 12;
    for (size_t i = start; i < start + 12 && i < sizeof(operations) / sizeof(*operations); ++i) {
        const operation *op = &operations[i];
        bool iot = !strncmp(op->name, "mqtt.", 5) || !strncmp(op->name, "espnow.", 7) ||
                   !strncmp(op->name, "power.", 6);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "op", op->name);
        cJSON_AddStringToObject(p, "params", op->keys);
        cJSON_AddBoolToObject(p, "available", !iot || !strcmp(profile, "iot"));
        v3_push(a, p);
    }
    return a;
}
const char *v3_operation_validate(const char *name, const cJSON *p) {
    const operation *op = NULL;
    for (size_t i = 0; i < sizeof(operations) / sizeof(*operations); ++i)
        if (!strcmp(operations[i].name, name))
            op = &operations[i];
    if (!op)
        return "unknown_operation";
    if (!v3_keys(p, op->keys))
        return "invalid_argument";
    static const char *const ids[] = {"resource", "job_id", "buffer_id", "upload_id", "flow_id"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); ++i)
        if (v3_get(p, ids[i]) && !v3_integer(v3_get(p, ids[i]), 1, 2147483647))
            return "invalid_argument";
    static const char *const bools[] = {"cascade", "reboot",           "persist",
                                        "enable",  "retain",           "will_retain",
                                        "encrypt", "auto_light_sleep", "secret"};
    for (size_t i = 0; i < sizeof(bools) / sizeof(*bools); ++i)
        if (v3_get(p, bools[i]) && !cJSON_IsBool(v3_get(p, bools[i])))
            return "invalid_argument";
    static const char *const strings[] = {"name",
                                          "kind",
                                          "action",
                                          "ssid",
                                          "password",
                                          "uri",
                                          "username",
                                          "ca_pem",
                                          "ca_file",
                                          "certificate_file",
                                          "private_key_file",
                                          "will_topic",
                                          "will",
                                          "topic",
                                          "mac",
                                          "key",
                                          "pmk",
                                          "sha256",
                                          "board",
                                          "confirm",
                                          "data",
                                          "mode",
                                          "modem_sleep"};
    for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); ++i)
        if (v3_get(p, strings[i]) && !cJSON_IsString(v3_get(p, strings[i])))
            return "invalid_argument";
    const char *const nonnegative[] = {"offset",   "length", "page",
                                       "revision", "size",   "duration_ms"};
    for (size_t i = 0; i < sizeof(nonnegative) / sizeof(*nonnegative); ++i)
        if (v3_get(p, nonnegative[i]) && !v3_integer(v3_get(p, nonnegative[i]), 0, 2147483647))
            return "invalid_argument";
    return NULL;
}
const char *v3_action_validate(v3_kind kind, const char *name, const cJSON *p) {
    if (!cJSON_IsObject(p))
        return "invalid_argument";
    const action *a = find(kind, name);
    if (!a && strcmp(name, "status") && strcmp(name, "events"))
        return "unsupported_operation";
    if (!strcmp(name, "events") && kind != V_GPIO && kind != V_ADC && kind != V_ADC_CONT &&
        kind != V_UART && kind != V_I2C_SLAVE && kind != V_TWAI)
        return "unsupported_operation";
    for (const cJSON *v = p->child; v; v = v->next) {
        if (!strcmp(v->string, "timeout_ms")) {
            if (!v3_integer(v, 0, V3_TIMEOUT_MAX))
                return "invalid_argument";
            continue;
        }
        const argument *arg = NULL;
        if (a)
            for (size_t i = 0; i < a->count; ++i)
                if (!strcmp(v->string, a->args[i].name))
                    arg = &a->args[i];
        if (!arg)
            return "invalid_argument";
        if (arg->type == 'i' && !v3_integer(v, arg->min, arg->max))
            return "invalid_argument";
        if (arg->type == 'n' && (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
                                 v->valuedouble < arg->min || v->valuedouble > arg->max))
            return "invalid_argument";
        if (arg->type == 's' && !cJSON_IsString(v))
            return "invalid_argument";
        if (arg->type == 'b' && !cJSON_IsBool(v))
            return "invalid_argument";
    }
    if (a) {
        for (size_t i = 0; i < a->count; ++i)
            if (a->args[i].required && !v3_get(p, a->args[i].name))
                return "invalid_argument";
        if (a->payload && (!v3_get(p, "data") == !v3_get(p, "buffer_id")))
            return "invalid_argument";
    }
    return NULL;
}
bool v3_action_readonly(v3_kind k, const char *a) {
    return !strcmp(a, "status") || (k == V_GPIO && !strcmp(a, "read")) ||
           (k == V_TEMPERATURE && !strcmp(a, "read")) || (k == V_TWAI && !strcmp(a, "info"));
}
cJSON *v3_actions_json(v3_kind kind) {
    cJSON *a = cJSON_CreateArray();
    for (size_t i = 0; i < sizeof(actions) / sizeof(*actions); ++i) {
        const action *v = &actions[i];
        if (v->kind != kind)
            continue;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "action", v->name);
        cJSON_AddBoolToObject(p, "payload", v->payload);
        cJSON *args = cJSON_AddArrayToObject(p, "args");
        for (size_t j = 0; j < v->count; ++j) {
            const argument *f = &v->args[j];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", f->name);
            cJSON_AddBoolToObject(item, "required", f->required);
            cJSON_AddStringToObject(item, "type",
                                    f->type == 'i'   ? "integer"
                                    : f->type == 'n' ? "number"
                                    : f->type == 'b' ? "boolean"
                                                     : "string");
            if (f->type == 'i' || f->type == 'n') {
                cJSON_AddNumberToObject(item, "min", f->min);
                cJSON_AddNumberToObject(item, "max", f->max);
            }
            v3_push(args, item);
        }
        v3_push(a, p);
    }
    return a;
}
