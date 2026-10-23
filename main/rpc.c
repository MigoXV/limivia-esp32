#include "v3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cJSON empty = {.type = cJSON_Object};
typedef struct {
    const char *text;
    size_t size;
} span;
typedef struct {
    const cJSON *context, *args;
    const char *method, *reason;
    span id;
    int code;
    bool notification;
} call;

// Only invoked on strict-parser validated JSON. Keep raw ID tokens to avoid
// rounding large numbers and to preserve escaped string IDs without re-parsing.
static size_t whitespace(const char *s, size_t i, size_t n) {
    while (i < n && strchr(" \r\n\t", s[i]))
        ++i;
    return i;
}
static size_t token_end(const char *s, size_t i, size_t n) {
    if (i >= n)
        return n;
    if (s[i] == '"') {
        while (++i < n) {
            if (s[i] == '\\')
                ++i;
            else if (s[i] == '"')
                return i + 1;
        }
        return n;
    }
    if (s[i] == '{' || s[i] == '[') {
        unsigned depth = 1;
        for (++i; i < n; ++i) {
            if (s[i] == '"')
                i = token_end(s, i, n) - 1;
            else if (s[i] == '{' || s[i] == '[')
                ++depth;
            else if ((s[i] == '}' || s[i] == ']') && !--depth)
                return i + 1;
        }
        return n;
    }
    while (i < n && !strchr(" \r\n\t,]}", s[i]))
        ++i;
    return i;
}
static span raw_id(const char *s, size_t n, size_t start, const cJSON *object) {
    size_t i = start + 1;
    for (const cJSON *p = object->child; p; p = p->next) {
        i = whitespace(s, i, n);
        i = whitespace(s, token_end(s, i, n), n) + 1; // colon
        i = whitespace(s, i, n);
        size_t end = token_end(s, i, n);
        if (!strcmp(p->string, "id"))
            return (span){s + i, end - i};
        i = whitespace(s, end, n) + 1; // comma
    }
    return (span){0};
}
static void reject(call *r, int code, const char *reason) {
    r->code = code;
    r->reason = reason;
}
static bool nonce_valid(const char *s) {
    return strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32;
}
static bool sequence(v3_session *s, uint32_t seq) {
    if (seq > s->sequence_high) {
        uint32_t shift = seq - s->sequence_high;
        s->sequence_seen = shift >= 64 ? 1 : (s->sequence_seen << shift) | 1;
        s->sequence_high = seq;
        return true;
    }
    uint32_t offset = s->sequence_high - seq;
    if (offset >= 64 || (s->sequence_seen & (UINT64_C(1) << offset)))
        return false;
    s->sequence_seen |= UINT64_C(1) << offset;
    return true;
}
static call prepare(v3_core *c, int endpoint, const cJSON *p, const char *s, size_t n,
                    size_t start, bool batch, bool retained) {
    call r = {.context = &empty, .args = &empty, .method = ""};
    const cJSON *id = v3_get(p, "id"), *method = v3_get(p, "method");
    if (!cJSON_IsObject(p) || strcmp(v3_str(p, "jsonrpc", ""), "2.0") ||
        !cJSON_IsString(method) ||
        (id && !cJSON_IsNull(id) && !cJSON_IsNumber(id) && !cJSON_IsString(id))) {
        reject(&r, -32600, "invalid_request");
        return r;
    }
    r.notification = !id;
    r.id = raw_id(s, n, start, p);
    r.method = method->valuestring;
    if (retained) {
        reject(&r, 1000, "retained_command");
        return r;
    }
    const cJSON *params = v3_get(p, "params");
    if (params && !cJSON_IsObject(params) && !cJSON_IsArray(params)) {
        reject(&r, -32600, "invalid_request");
        return r;
    }
    if (cJSON_IsArray(params)) {
        if (cJSON_GetArraySize(params) > 2) {
            reject(&r, -32602, "invalid_argument");
            return r;
        }
        r.context = cJSON_GetArrayItem(params, 0);
        r.args = cJSON_GetArrayItem(params, 1);
    } else if (params) {
        if (!v3_keys(params, "context args")) {
            reject(&r, -32602, "invalid_argument");
            return r;
        }
        r.context = v3_get(params, "context");
        r.args = v3_get(params, "args");
    }
    if (!r.context)
        r.context = &empty;
    if (!r.args)
        r.args = &empty;
    if (!cJSON_IsObject(r.context) || !cJSON_IsObject(r.args) ||
        !v3_keys(r.context, "session control seq client_nonce")) {
        reject(&r, -32602, "invalid_argument");
        return r;
    }
    const char *error = v3_operation_validate(r.method, r.args);
    if (error) {
        reject(&r, !strcmp(error, "unknown_operation") ? -32601 : -32602, error);
        return r;
    }
    if (!strcmp(r.method, "hello")) {
        if (batch)
            reject(&r, 1000, "handshake_must_be_single");
        else if (!v3_keys(r.context, "client_nonce") ||
                 !nonce_valid(v3_str(r.context, "client_nonce", "")))
            reject(&r, -32602, "invalid_argument");
        return r;
    }
    v3_session *session = &c->sessions[endpoint];
    if (!session->token[0] || strcmp(session->token, v3_str(r.context, "session", "")))
        reject(&r, 1000, "invalid_session");
    else if (!v3_integer(v3_get(r.context, "seq"), 1, 2147483647) ||
             (v3_get(r.context, "control") && !cJSON_IsString(v3_get(r.context, "control"))))
        reject(&r, -32602, "invalid_argument");
    else if (!sequence(session, v3_int(r.context, "seq", 0)))
        reject(&r, 1002, "duplicate_request");
    else
        session->expires = c->now + 5000;
    return r;
}
static cJSON *context(const v3_core *c, int endpoint) {
    cJSON *p = cJSON_CreateObject();
    if (!p || !cJSON_AddStringToObject(p, "boot_id", c->boot) ||
        !cJSON_AddStringToObject(p, "session", c->sessions[endpoint].token)) {
        cJSON_Delete(p);
        return NULL;
    }
    return p;
}
static cJSON *hello(v3_core *c, int endpoint, const call *r) {
    v3_session *s = &c->sessions[endpoint];
    const char *nonce = v3_str(r->context, "client_nonce", "");
    if (!s->token[0] || strcmp(s->client_nonce, nonce)) {
        if (c->owner == endpoint) {
            c->owner = -1;
            c->control[0] = 0;
        }
        uint32_t generation = s->generation;
        memset(s, 0, sizeof(*s));
        s->generation = generation;
        snprintf(s->client_nonce, sizeof(s->client_nonce), "%s", nonce);
        memcpy(s->token, c->boot, 16);
        snprintf(s->token + 16, sizeof(s->token) - 16, "-s%08lx", (unsigned long)++c->nonce);
        s->event_interval_ms = 100;
    }
    s->expires = c->now + 5000;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "session", s->token);
    cJSON_AddStringToObject(p, "device_id", c->device);
    cJSON_AddStringToObject(p, "boot_id", c->boot);
    cJSON_AddStringToObject(p, "board", "luatos-esp32c3-usb");
    cJSON_AddStringToObject(p, "firmware", "0.3.0-dev");
    cJSON_AddStringToObject(p, "build_id", c->build);
    cJSON_AddStringToObject(p, "profile", c->profile);
    cJSON_AddStringToObject(p, "jsonrpc", "2.0");
    cJSON_AddStringToObject(p, "contract", LV_RPC_CONTRACT);
    cJSON_AddNumberToObject(p, "protocol", LV_PROTOCOL);
    cJSON_AddNumberToObject(p, "request_max", LV_REQUEST_MAX);
    cJSON_AddNumberToObject(p, "response_max", LV_RESPONSE_MAX);
    cJSON_AddNumberToObject(p, "batch_max", LV_RPC_BATCH_MAX);
    cJSON_AddNumberToObject(p, "sequence_window", 64);
    cJSON_AddNumberToObject(p, "sequence_high", s->sequence_high);
    cJSON_AddNumberToObject(p, "session_timeout_ms", 5000);
    cJSON_AddStringToObject(p, "auth_mode", "open");
    cJSON_AddStringToObject(p, "binary", "base64_chunks");
    cJSON_AddNumberToObject(p, "buffer_max", V3_BUFFER_MAX);
    cJSON_AddNumberToObject(p, "resources_max", V3_RESOURCES);
    cJSON_AddNumberToObject(p, "job_history", V3_JOBS);
    cJSON_AddNumberToObject(p, "capability_pages", v3_capability_pages());
    cJSON_AddStringToObject(p, "features",
                            "resources jobs buffers flows events gpio_matrix storage wifi websocket ota");
    cJSON_AddBoolToObject(p, "iot_services", !strcmp(c->profile, "iot"));
    return p;
}
static int domain_code(const char *error) {
    if (!strcmp(error, "invalid_argument"))
        return -32602;
    if (!strcmp(error, "unknown_operation"))
        return -32601;
    size_t n = strlen(error);
    if (!strcmp(error, "capacity") || !strcmp(error, "busy") ||
        (n >= 9 && !strcmp(error + n - 9, "_capacity")) ||
        (n >= 5 && !strcmp(error + n - 5, "_busy")))
        return 1001;
    return 1000;
}
static const char *message(int code) {
    switch (code) {
    case -32700: return "Parse error";
    case -32600: return "Invalid Request";
    case -32601: return "Method not found";
    case -32602: return "Invalid params";
    case -32603: return "Internal error";
    case 1001: return "Capacity exceeded";
    case 1002: return "Duplicate or stale sequence";
    default: return "Operation rejected";
    }
}
static cJSON *identifier(span id) {
    if (!id.text)
        return cJSON_CreateNull();
    char *text = cJSON_malloc(id.size + 1);
    if (!text)
        return NULL;
    memcpy(text, id.text, id.size);
    text[id.size] = 0;
    cJSON *p = cJSON_CreateRaw(text);
    cJSON_free(text);
    return p;
}
static cJSON *reply(v3_core *c, int endpoint, const call *r, cJSON *value) {
    cJSON *p = cJSON_CreateObject(), *body = cJSON_CreateObject();
    bool valid = p && body && cJSON_AddStringToObject(p, "jsonrpc", "2.0") &&
                 v3_put(p, "id", identifier(r->id));
    if (r->code) {
        cJSON *data = cJSON_CreateObject();
        valid = valid && data && cJSON_AddNumberToObject(body, "code", r->code) &&
                cJSON_AddStringToObject(body, "message", message(r->code)) &&
                cJSON_AddStringToObject(data, "reason", r->reason) &&
                v3_put(data, "context", context(c, endpoint));
        if (!v3_put(body, "data", data))
            valid = false;
        cJSON_Delete(value);
    } else {
        valid = valid && v3_put(body, "context", context(c, endpoint));
        if (!v3_put(body, "value", value))
            valid = false;
    }
    if (!v3_put(p, r->code ? "error" : "result", body))
        valid = false;
    if (!valid) {
        cJSON_Delete(p);
        return NULL;
    }
    return p;
}
// No allocations: works even when encoding failed after a physical side effect.
static size_t capacity(v3_core *c, int endpoint, span id, uint32_t job, char *out, size_t cap) {
    const char *prefix = "{\"jsonrpc\":\"2.0\",\"id\":";
    size_t used = strlen(prefix), length = id.text ? id.size : 4;
    if (out) {
        if (used + length + 1 >= cap)
            return 0;
        memcpy(out, prefix, used);
        memcpy(out + used, id.text ? id.text : "null", length);
    }
    used += length;
    int n = snprintf(out ? out + used : NULL, out ? cap - used : 0,
                     ",\"error\":{\"code\":1001,\"message\":\"Capacity exceeded\",\"data\":{"
                     "\"reason\":\"response_capacity\",\"context\":{\"boot_id\":\"%s\",\"session\":\"%s\"},"
                     "\"external_effect_possible\":true,\"job_id\":%lu}}}",
                     c->boot, c->sessions[endpoint].token, (unsigned long)job);
    return n < 0 || (out && (size_t)n >= cap - used) ? 0 : used + n;
}
static bool changes(v3_core *c, const call *r) {
    return !v3_readonly(c, r->method, r->args) || !strcmp(r->method, "control.acquire") ||
           !strcmp(r->method, "event.subscribe") || !strcmp(r->method, "event.unsubscribe");
}
typedef struct {
    char *text;
    size_t cap;
    bool grow, allocation_failed;
} rpc_buffer;
static bool expand(rpc_buffer *b, size_t minimum) {
    if (minimum <= b->cap)
        return true;
    if (!b->grow || minimum > LV_RESPONSE_MAX + 2)
        return false;
    size_t cap = b->cap;
    while (cap < minimum) {
        cap *= 2;
        if (cap > LV_RESPONSE_MAX + 2)
            cap = LV_RESPONSE_MAX + 2;
    }
    char *p = realloc(b->text, cap);
    if (!p) {
        b->allocation_failed = true;
        return false;
    }
    b->text = p;
    b->cap = cap;
    return true;
}
static size_t encode_message(v3_core *c, int endpoint, uint32_t generation,
                              const char *text, size_t n, int64_t now, bool retained,
                              rpc_buffer *b) {
    if (endpoint < 0 || endpoint >= V3_ENDPOINTS || n > LV_REQUEST_MAX || b->cap < 2)
        return 0;
    if (b->cap > LV_RESPONSE_MAX + 2)
        b->cap = LV_RESPONSE_MAX + 2;
    v3_tick(c, now);
    v3_disconnect(c, endpoint, generation);
    unsigned parse_start = v3_json_failures();
    cJSON *root = v3_parse(text, n);
    bool batch = cJSON_IsArray(root);
    int count = batch ? cJSON_GetArraySize(root) : 1;
    if (count > LV_RPC_BATCH_MAX) {
        cJSON_Delete(root);
        return 0; // transport admission limit: nothing has executed
    }
    call calls[LV_RPC_BATCH_MAX] = {0};
    size_t offset = whitespace(text, 0, n);
    if (batch && count)
        offset = whitespace(text, offset + 1, n);
    if (!root) {
        reject(&calls[0], v3_json_failures() != parse_start ? 1001 : -32700,
               v3_json_failures() != parse_start ? "capacity" : "parse_error");
    } else if (!count) {
        batch = false;
        count = 1;
        reject(&calls[0], -32600, "invalid_request");
    } else {
        for (int i = 0; i < count; ++i) {
            const cJSON *p = batch ? cJSON_GetArrayItem(root, i) : root;
            calls[i] = prepare(c, endpoint, p, text, n, offset, batch, retained);
            offset = whitespace(text, token_end(text, offset, n), n) + 1;
            offset = whitespace(text, offset, n);
        }
    }
    int first = 0;
    bool barrier = false;
    for (int i = 0; i < count; ++i)
        if (!calls[i].code && !strcmp(calls[i].method, "outputs.stop")) {
            first = i;
            barrier = true;
            break;
        }
    // Reserve an allocation-free error for every response before any execution.
    size_t reserved[LV_RPC_BATCH_MAX] = {0}, remaining = 0;
    for (int i = 0; i < count; ++i)
        if (!calls[i].notification) {
            reserved[i] = capacity(c, endpoint, calls[i].id, UINT32_MAX, NULL, 0) + V3_TOKEN + 1;
            remaining += reserved[i];
        }
    if (!expand(b, remaining + 3)) {
        cJSON_Delete(root);
        return 0;
    }
    size_t used = batch ? 1 : 0;
    if (batch)
        b->text[0] = '[';
    int replies = 0;
    uint32_t batch_epoch = c->epoch;
    for (int step = 0; step < count; ++step) {
        int i = step == 0 ? first : step <= first ? step - 1 : step;
        call *r = &calls[i];
        if (batch && step && c->adapter.interrupt)
            c->adapter.interrupt(c->adapter.context);
        if (batch && c->epoch != batch_epoch)
            barrier = true;
        unsigned allocation_start = v3_json_failures();
        cJSON *value = NULL;
        uint32_t job = 0;
        if (!r->code) {
            if (strcmp(r->method, "hello") &&
                strcmp(c->sessions[endpoint].token, v3_str(r->context, "session", "")))
                reject(r, 1000, "invalid_session");
            else if (barrier && strcmp(r->method, "outputs.stop") && changes(c, r))
                reject(r, 1000, "stop_in_progress");
            else if (!strcmp(r->method, "hello"))
                value = hello(c, endpoint, r);
            else {
                const char *error = NULL;
                value = v3_dispatch(c, endpoint, r->method, r->args,
                                    v3_str(r->context, "control", ""), &error);
                if (error)
                    reject(r, domain_code(error), error);
            }
        }
        if (r->code)
            ++c->rejected;
        job = v3_int(value, "job_id", 0);
        if (r->notification) {
            cJSON_Delete(value);
            continue;
        }
        remaining -= reserved[i];
        if (batch && replies)
            b->text[used++] = ',';
        cJSON *p = reply(c, endpoint, r, value);
        bool valid = p && allocation_start == v3_json_failures();
        size_t length = 0;
        do {
            bool encoded = valid && cJSON_PrintPreallocated(p, b->text + used,
                                                             (int)(b->cap - used), false);
            length = encoded ? strlen(b->text + used) : 0;
            if (length && length + used + remaining + 3 <= b->cap)
                break;
            // Re-encode this retained result after growing, never re-dispatch a call.
        } while (valid && b->cap < LV_RESPONSE_MAX + 2 && expand(b, b->cap + 1));
        cJSON_Delete(p);
        size_t available = b->cap - used - remaining - 3;
        if (!length || length > available)
            length = capacity(c, endpoint, r->id, job, b->text + used, b->cap - used);
        if (length > available || !length) {
            cJSON_Delete(root);
            return 0;
        }
        used += length;
        ++replies;
    }
    cJSON_Delete(root);
    if (!replies)
        return 0;
    if (batch)
        b->text[used++] = ']';
    b->text[used++] = '\n';
    b->text[used] = 0;
    return used;
}
size_t v3_rpc_wire(v3_core *c, int endpoint, uint32_t generation, const char *text, size_t n,
                   int64_t now, bool retained, char *out, size_t cap) {
    rpc_buffer b = {.text = out, .cap = cap};
    return encode_message(c, endpoint, generation, text, n, now, retained, &b);
}
char *v3_rpc_alloc(v3_core *c, int endpoint, uint32_t generation, const char *text, size_t n,
                    int64_t now, bool retained, size_t *size) {
    rpc_buffer b = {.text = malloc(512), .cap = 512, .grow = true};
    if (!b.text) {
        *size = SIZE_MAX;
        return NULL;
    }
    *size = encode_message(c, endpoint, generation, text, n, now, retained, &b);
    if (!*size) {
        free(b.text);
        if (b.allocation_failed)
            *size = SIZE_MAX;
        return NULL;
    }
    return b.text;
}
int v3_rpc_priority(const char *text, size_t n) {
    cJSON *root = v3_parse(text, n);
    bool batch = cJSON_IsArray(root);
    int count = batch ? cJSON_GetArraySize(root) : 1;
    int priority = 1;
    if (count > LV_RPC_BATCH_MAX)
        priority = -1;
    else
        for (int i = 0; i < count; ++i) {
            const cJSON *p = batch ? cJSON_GetArrayItem(root, i) : root;
            const char *op = v3_str(p, "method", "");
            if (!strcmp(op, "outputs.stop")) {
                priority = 2;
                break;
            }
            if (strcmp(op, "hello") && strcmp(op, "session.ping") && strcmp(op, "job.get"))
                priority = 0;
        }
    cJSON_Delete(root);
    return priority;
}
cJSON *v3_rpc_notice(const v3_core *c, int endpoint, const char *method, cJSON *args) {
    cJSON *p = cJSON_CreateObject(), *params = cJSON_CreateObject();
    bool valid = p && params && args && cJSON_AddStringToObject(p, "jsonrpc", "2.0") &&
                 cJSON_AddStringToObject(p, "method", method) &&
                 v3_put(params, "context", context(c, endpoint));
    if (!v3_put(params, "args", args))
        valid = false;
    if (!v3_put(p, "params", params))
        valid = false;
    if (!valid) {
        cJSON_Delete(p);
        return NULL;
    }
    return p;
}
// Native callers share the production wire path rather than a second dispatcher.
cJSON *v3_handle_message(v3_core *c, int endpoint, uint32_t generation, const char *text,
                         size_t n, int64_t now, bool retained) {
    char *out = malloc(LV_RESPONSE_MAX + 8);
    if (!out)
        return NULL;
    size_t size = v3_rpc_wire(c, endpoint, generation, text, n, now, retained,
                              out, LV_RESPONSE_MAX + 8);
    cJSON *p = size ? v3_parse(out, size) : NULL;
    free(out);
    return p;
}
cJSON *v3_handle(v3_core *c, int endpoint, uint32_t generation, const char *text,
                 size_t n, int64_t now) {
    return v3_handle_message(c, endpoint, generation, text, n, now, false);
}
