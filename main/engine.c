#include "v3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void v3_init(v3_core *c, v3_adapter a, const char *boot, const char *device, const char *build,
             const char *profile) {
    memset(c, 0, sizeof(*c));
    snprintf(c->boot, sizeof(c->boot), "%s", boot);
    snprintf(c->device, sizeof(c->device), "%s", device);
    snprintf(c->build, sizeof(c->build), "%s", build);
    snprintf(c->profile, sizeof(c->profile), "%s", profile);
    c->owner = -1;
    c->epoch = 1;
    c->next_id = 1;
    c->adapter = a;
}
void v3_work_free(v3_work *w) {
    if (w) {
        cJSON_Delete(w->definition);
        cJSON_Delete(w->params);
        free(w);
    }
}
void v3_destroy(v3_core *c) {
    v3_flow_destroy(c);
    for (int i = 0; i < V3_RESOURCES; ++i)
        cJSON_Delete(c->resources[i].definition);
    for (int i = 0; i < V3_JOBS; ++i)
        cJSON_Delete(c->jobs[i].result);
    for (int i = 0; i < V3_BUFFERS; ++i)
        free(c->buffers[i].data);
}
bool v3_idle(const v3_core *c) {
    for (int i = 0; i < V3_RESOURCES; ++i)
        if (c->resources[i].state != V_FREE)
            return false;
    return true;
}
bool v3_exclusive_pending(const v3_core *c) {
    for (int i = 0; i < V3_JOBS; ++i) {
        const v3_job *j = &c->jobs[i];
        if (j->state != J_QUEUED)
            continue;
        if ((!strncmp(j->action, "ota.", 4) && strcmp(j->action, "ota.status")) ||
            !strncmp(j->action, "power.", 6) || !strcmp(j->action, "storage.format") ||
            !strcmp(j->action, "system.nvs_format"))
            return true;
    }
    return false;
}
void v3_disconnect(v3_core *c, int endpoint, uint32_t generation) {
    if (endpoint < 0 || endpoint >= V3_ENDPOINTS)
        return;
    v3_session *s = &c->sessions[endpoint];
    if (s->generation == generation)
        return;
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    if (c->owner == endpoint) {
        c->owner = -1;
        c->control[0] = 0;
    }
}
v3_job *v3_job_find(v3_core *c, uint32_t id) {
    for (int i = 0; i < V3_JOBS; ++i)
        if (c->jobs[i].state != J_FREE && c->jobs[i].id == id)
            return &c->jobs[i];
    return NULL;
}
bool v3_job_cancel(v3_core *c, uint32_t id) {
    v3_job *j = v3_job_find(c, id);
    if (!j || j->state != J_QUEUED || !j->work || !strcmp(j->action, "close"))
        return false;
    atomic_store(&j->work->cancelled, true);
    return true;
}
static v3_job *new_job(v3_core *c, const char *action) {
    v3_job *old = NULL;
    int first = !strcmp(action, "outputs.stop") ? V3_JOBS - 1 : 0;
    int end = first ? V3_JOBS : V3_JOBS - 1;
    for (int i = first; i < end; ++i) {
        v3_job *j = &c->jobs[i];
        if (j->state == J_FREE) {
            old = j;
            break;
        }
        if (j->state != J_QUEUED && !j->reply_pending && (!old || old->id > j->id))
            old = j;
    }
    if (!old || c->next_id >= 2147483647)
        return NULL;
    cJSON_Delete(old->result);
    c->retained_result_bytes -= old->result_bytes;
    memset(old, 0, sizeof(*old));
    old->id = c->next_id++;
    old->epoch = c->epoch;
    old->state = J_QUEUED;
    snprintf(old->action, sizeof(old->action), "%s", action);
    return old;
}
static cJSON *job_json(const v3_job *j) {
    static const char *states[] = {"free", "accepted", "completed", "failed", "cancelled"};
    cJSON *p = cJSON_CreateObject();
    v3_add_u32(p, "job_id", j->id);
    v3_add_u32(p, "resource", j->resource);
    cJSON_AddStringToObject(p, "state", states[j->state]);
    cJSON_AddStringToObject(p, "action", j->action);
    cJSON_AddNumberToObject(p, "driver_error", j->error);
    if (j->error)
        cJSON_AddStringToObject(p, "error_code",
                                v3_str(j->result, "error_code",
                                       j->state == J_CANCELLED ? "cancelled" : "driver_error"));
    cJSON_AddBoolToObject(p, "external_effect_possible", j->external_effect);
    cJSON_AddBoolToObject(p, "result_expired", j->result_expired);
    cJSON_AddBoolToObject(p, "cancel_requested", j->work && atomic_load(&j->work->cancelled));
    if (j->result)
        v3_put(p, "result", cJSON_Duplicate(j->result, true));
    return p;
}
v3_job *v3_schedule(v3_core *c, v3_resource *r, const char *action, const cJSON *params) {
    v3_job *j = new_job(c, action);
    if (!j)
        return NULL;
    v3_work *w = calloc(1, sizeof(*w));
    if (!w) {
        j->state = J_FREE;
        return NULL;
    }
    w->job = j->id;
    w->epoch = c->epoch;
    w->slot = -1;
    w->parent_slot = -1;
    snprintf(w->action, sizeof(w->action), "%s", action);
    w->params = params ? cJSON_Duplicate(params, true) : cJSON_CreateObject();
    if (r) {
        j->resource = r->id;
        j->instance = r->instance;
        w->resource = r->id;
        w->instance = r->instance;
        w->slot = (int)(r - c->resources);
        w->kind = r->kind;
        w->controller = r->controller;
        w->channel = r->channel;
        w->timer = r->timer;
        v3_resource *parent = v3_find(c, r->parent);
        if (parent)
            w->parent_slot = parent - c->resources;
        w->definition = cJSON_Duplicate(r->definition, true);
    }
    if (!w->params || (r && !w->definition) || !c->adapter.enqueue ||
        !c->adapter.enqueue(c->adapter.context, w)) {
        v3_work_free(w);
        j->state = J_FREE;
        return NULL;
    }
    if (r) {
        r->busy = j->id;
        v3_resource *p = v3_find(c, r->parent);
        if (p)
            p->busy = j->id;
        ++c->revision;
    }
    j->work = w;
    return j;
}
static bool children(const v3_core *c, const v3_resource *r) {
    for (int i = 0; i < V3_RESOURCES; ++i)
        if (c->resources[i].state != V_FREE && c->resources[i].parent == r->id)
            return true;
    return false;
}
void v3_close_tree(v3_core *c, v3_resource *r) {
    for (int i = 0; i < V3_RESOURCES; ++i)
        if (c->resources[i].state != V_FREE && c->resources[i].parent == r->id)
            v3_close_tree(c, &c->resources[i]);
    r->state = V_CLOSING;
    r->cleanup_error = 0;
}
static size_t json_bytes(const cJSON *p) {
    if (!p)
        return 0;
    size_t n = sizeof(*p) + 2 * sizeof(void *);
    if (p->string)
        n += strlen(p->string) + 1 + 2 * sizeof(void *);
    if (cJSON_IsString(p))
        n += strlen(p->valuestring) + 1 + 2 * sizeof(void *);
    for (const cJSON *v = p->child; v; v = v->next)
        n += json_bytes(v);
    return n;
}
void v3_complete(v3_core *c, v3_work *w, int error, bool external, cJSON *result) {
    v3_job *j = v3_job_find(c, w->job);
    v3_resource *r = v3_find(c, w->resource);
    bool current = w->epoch == c->epoch;
    if (!j || j->epoch != w->epoch || j->instance != w->instance) {
        ++c->stale;
        cJSON_Delete(result);
        return;
    }
    if (current && !error && !strcmp(w->action, "flow.load")) {
        uint32_t id = 0;
        const char *failure =
            v3_flow_install(c, v3_str(result, "name", ""), v3_get(result, "definition"), &id);
        cJSON_Delete(result);
        result = cJSON_CreateObject();
        if (failure) {
            error = 1;
            cJSON_AddStringToObject(result, "error", failure);
        } else
            v3_add_u32(result, "flow_id", id);
    }
    j->error = error;
    j->work = NULL;
    j->external_effect = external;
    if (!current || atomic_load(&w->cancelled)) {
        j->state = J_CANCELLED;
        ++c->stale;
        cJSON_Delete(result);
        result = NULL;
    } else {
        j->state = error ? J_FAILED : J_DONE;
        j->result = result;
        j->result_bytes = json_bytes(result);
        for (int i = 0; i < V3_JOBS && c->retained_result_bytes + j->result_bytes > 32768; ++i) {
            v3_job *old = &c->jobs[i];
            if (old == j || !old->result || old->state == J_QUEUED || old->reply_pending)
                continue;
            c->retained_result_bytes -= old->result_bytes;
            old->result_bytes = 0;
            cJSON_Delete(old->result);
            old->result = NULL;
            old->result_expired = true;
        }
        if (j->result_bytes > 32768) {
            cJSON_Delete(j->result);
            j->result = NULL;
            j->result_expired = true;
            j->result_bytes = 0;
        }
        c->retained_result_bytes += j->result_bytes;
    }
    if (!r || r->instance != w->instance)
        return;
    if (r->busy != j->id)
        return;
    r->busy = 0;
    for (int i = 0; i < V3_RESOURCES; ++i)
        if (c->resources[i].busy == j->id)
            c->resources[i].busy = 0;
    v3_resource *p = v3_find(c, r->parent);
    if (p && p->busy == j->id)
        p->busy = 0;
    if (!strcmp(w->action, "close")) {
        if (error) {
            r->state = V_FAULT;
            r->cleanup_error = error;
        } else {
            cJSON_Delete(r->definition);
            memset(r, 0, sizeof(*r));
        }
    } else if (current && !strcmp(w->action, "open") && r->state != V_CLOSING) {
        r->state = error ? V_FAULT : V_READY;
        r->error = error;
    }
    // Transactions may return NACK/no-data/timeout without poisoning an initialized controller.
    // A stop during open retains the reservation until an explicit close completes.
    ++c->revision;
    j->revision = c->revision;
    j->observed_us = c->now * 1000;
}
void v3_tick(v3_core *c, int64_t now) {
    c->now = now;
    for (int i = 0; i < V3_ENDPOINTS; ++i)
        if (c->sessions[i].token[0] && c->sessions[i].expires <= now) {
            c->sessions[i].token[0] = 0;
            if (c->owner == i) {
                c->owner = -1;
                c->control[0] = 0;
            }
        }
    for (int i = 0; i < V3_RESOURCES; ++i) {
        v3_resource *r = &c->resources[i];
        if (r->state == V_CLOSING && !r->busy && children(c, r)) {
            for (int j = 0; j < V3_RESOURCES; ++j) {
                const v3_resource *child = &c->resources[j];
                if (child->parent == r->id && child->state == V_FAULT && !child->busy) {
                    r->state = V_FAULT;
                    r->cleanup_error = 1;
                    break;
                }
            }
        }
        if (r->state == V_CLOSING && !r->busy && !children(c, r))
            v3_schedule(c, r, "close", NULL);
    }
    if (c->stopping) {
        bool pending = false;
        int faults = 0, persist_error = 0;
        for (int i = 0; i < V3_RESOURCES; ++i) {
            v3_resource *r = &c->resources[i];
            if (r->busy || r->state == V_CLOSING)
                pending = true;
            if (r->state == V_FAULT)
                ++faults;
        }
        if (c->adapter.service) {
            const char *ignored = NULL;
            cJSON *status =
                c->adapter.service(c->adapter.context, "stop.persisted", NULL, &ignored);
            if (status) {
                pending |= !v3_bool(status, "done", false);
                persist_error = v3_int(status, "error", 0);
            }
            cJSON_Delete(status);
        }
        if (!pending) {
            c->stopping = false;
            for (int i = 0; i < V3_JOBS; ++i) {
                v3_job *j = &c->jobs[i];
                if (j->state == J_QUEUED && !strcmp(j->action, "outputs.stop")) {
                    j->state = faults || persist_error ? J_FAILED : J_DONE;
                    j->result = cJSON_CreateObject();
                    cJSON_AddNumberToObject(j->result, "cleanup_faults", faults);
                    cJSON_AddNumberToObject(j->result, "persist_error", persist_error);
                    cJSON_AddBoolToObject(j->result, "autostart_inhibited", true);
                }
            }
        }
    }
    v3_flow_tick(c);
}
static cJSON *state_json(v3_core *c, const cJSON *params, const char **error) {
    int page = v3_int(params, "page", 0);
    uint32_t rev = v3_int(params, "revision", 0);
    if (page < 0 || page >= 6) {
        *error = "invalid_argument";
        return NULL;
    }
    if (rev && rev != c->revision) {
        *error = "snapshot_expired";
        return NULL;
    }
    cJSON *p = cJSON_CreateObject();
    v3_add_u32(p, "revision", c->revision);
    v3_add_u32(p, "epoch", c->epoch);
    cJSON_AddNumberToObject(p, "page", page);
    cJSON_AddNumberToObject(p, "pages", 6);
    cJSON_AddNumberToObject(p, "uptime_ms", (double)c->now);
    cJSON_AddBoolToObject(p, "stopping", c->stopping);
    cJSON_AddBoolToObject(p, "autostart_inhibited", c->autostart_inhibited);
    cJSON_AddNumberToObject(p, "owner_endpoint", c->owner);
    cJSON *a = cJSON_AddArrayToObject(p, "resources");
    for (int i = page * 4; i < page * 4 + 4; ++i)
        if (c->resources[i].state != V_FREE)
            v3_push(a, v3_resource_json(&c->resources[i]));
    return p;
}
static cJSON *buffer_op(v3_core *c, const char *op, const cJSON *p, const char **error) {
    if (!strcmp(op, "buffer.create")) {
        if (!v3_integer(v3_get(p, "size"), 1, V3_BUFFER_MAX)) {
            *error = "invalid_argument";
            return NULL;
        }
        for (int i = 0; i < V3_BUFFERS; ++i)
            if (!c->buffers[i].id) {
                v3_buffer *b = &c->buffers[i];
                b->size = v3_int(p, "size", 0);
                b->data = calloc(1, b->size);
                if (!b->data) {
                    *error = "capacity";
                    return NULL;
                }
                b->id = c->next_id++;
                cJSON *r = cJSON_CreateObject();
                v3_add_u32(r, "buffer_id", b->id);
                cJSON_AddNumberToObject(r, "capacity", b->size);
                return r;
            }
        *error = "capacity";
        return NULL;
    }
    v3_buffer *b = NULL;
    for (int i = 0; i < V3_BUFFERS; ++i)
        if (c->buffers[i].id == (uint32_t)v3_int(p, "buffer_id", 0) && c->buffers[i].id)
            b = &c->buffers[i];
    if (!b) {
        *error = "buffer_not_found";
        return NULL;
    }
    if (!strcmp(op, "buffer.delete")) {
        free(b->data);
        memset(b, 0, sizeof(*b));
        return cJSON_CreateObject();
    }
    if (!v3_integer(v3_get(p, "offset"), 0, b->size)) {
        *error = "invalid_argument";
        return NULL;
    }
    size_t offset = v3_int(p, "offset", 0);
    if (!strcmp(op, "buffer.write")) {
        const char *text = v3_str(p, "data", NULL);
        size_t n;
        // Decode into scratch: malformed trailing data must not partially mutate the buffer.
        uint8_t temp[768];
        if (!text || !v3_decode64(text, temp, sizeof(temp), &n) || n > b->size - offset) {
            *error = "invalid_base64_or_range";
            return NULL;
        }
        memcpy(b->data + offset, temp, n);
        if (b->used < offset + n)
            b->used = offset + n;
    } else if (!strcmp(op, "buffer.read")) {
        if (offset > b->used || !v3_integer(v3_get(p, "length"), 0, V3_BUFFER_MAX)) {
            *error = "invalid_argument";
            return NULL;
        }
        size_t n = v3_int(p, "length", 0);
        if (n > b->used - offset)
            n = b->used - offset;
        char *text = v3_encode64(b->data + offset, n);
        if (!text) {
            *error = "capacity";
            return NULL;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "data", text);
        free(text);
        cJSON_AddNumberToObject(r, "offset", offset);
        cJSON_AddNumberToObject(r, "length", n);
        cJSON_AddNumberToObject(r, "used", b->used);
        return r;
    } else {
        *error = "unknown_operation";
        return NULL;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "used", b->used);
    return r;
}
static cJSON *operate(v3_core *c, int endpoint, const char *op, const cJSON *p,
                      const char **error) {
    if (!strcmp(op, "gpio.group.read") || !strcmp(op, "gpio.group.write") ||
        !strcmp(op, "rmt.sync")) {
        bool sync = !strcmp(op, "rmt.sync"), write = !strcmp(op, "gpio.group.write");
        const cJSON *items = v3_get(p, "resources");
        int count = cJSON_GetArraySize(items);
        if (!cJSON_IsArray(items) || count < 1 || count > (sync ? 2 : 11) || (sync && count != 2)) {
            *error = "invalid_argument";
            return NULL;
        }
        v3_resource *resources[11];
        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(items, i);
            if (!v3_keys(item, sync    ? "resource instance data"
                               : write ? "resource instance level"
                                       : "resource instance") ||
                !v3_integer(v3_get(item, "resource"), 1, 2147483647) ||
                !v3_integer(v3_get(item, "instance"), 1, 2147483647) ||
                (write && !v3_integer(v3_get(item, "level"), 0, 1)) ||
                (sync && !cJSON_IsString(v3_get(item, "data")))) {
                *error = "invalid_argument";
                return NULL;
            }
            resources[i] = v3_find(c, v3_int(item, "resource", 0));
            v3_resource *r = resources[i];
            if (!r || r->instance != (uint32_t)v3_int(item, "instance", 0) ||
                r->kind != (sync ? V_RMT_TX : V_GPIO) || r->state != V_READY) {
                *error = "invalid_resource";
                return NULL;
            }
            if (r->busy || ((sync || write) && r->flow)) {
                *error = "resource_busy";
                return NULL;
            }
            if (write &&
                !strcmp(v3_str(v3_get(r->definition, "config"), "mode", "input"), "input")) {
                *error = "invalid_mode";
                return NULL;
            }
            for (int j = 0; j < i; ++j)
                if (resources[j] == r) {
                    *error = "duplicate_resource";
                    return NULL;
                }
        }
        cJSON *args = cJSON_Duplicate(p, true);
        if (!args) {
            *error = "capacity";
            return NULL;
        }
        cJSON *slots = cJSON_AddArrayToObject(args, "slots");
        for (int i = 0; i < count; ++i)
            v3_push(slots, cJSON_CreateNumber(resources[i] - c->resources));
        v3_job *j = v3_schedule(c, resources[0],
                                sync    ? "sync"
                                : write ? "group.write"
                                        : "group.read",
                                args);
        cJSON_Delete(args);
        if (!j) {
            *error = "busy";
            return NULL;
        }
        for (int i = 0; i < count; ++i)
            resources[i]->busy = j->id;
        return job_json(j);
    }
    if (!strncmp(op, "flow.", 5) && strcmp(op, "flow.load"))
        return v3_flow_operation(c, op, p, error);
    if (!strcmp(op, "session.ping"))
        return cJSON_CreateObject();
    if (!strcmp(op, "event.subscribe") || !strcmp(op, "event.unsubscribe")) {
        v3_session *s = &c->sessions[endpoint];
        uint32_t mask = 0;
        if (!strcmp(op, "event.subscribe")) {
            const cJSON *topics = v3_get(p, "topics");
            if (!cJSON_IsArray(topics) || cJSON_GetArraySize(topics) > 4) {
                *error = "invalid_argument";
                return NULL;
            }
            for (const cJSON *t = topics->child; t; t = t->next) {
                if (!cJSON_IsString(t)) {
                    *error = "invalid_argument";
                    return NULL;
                }
                unsigned bit = !strcmp(t->valuestring, "job")        ? 1
                               : !strcmp(t->valuestring, "resource") ? 2
                               : !strcmp(t->valuestring, "flow")     ? 4
                               : !strcmp(t->valuestring, "data")     ? 8
                                                                     : 0;
                if (!bit) {
                    *error = "invalid_argument";
                    return NULL;
                }
                mask |= bit;
            }
            if (v3_get(p, "interval_ms") && !v3_integer(v3_get(p, "interval_ms"), 10, 60000)) {
                *error = "invalid_argument";
                return NULL;
            }
        }
        s->event_mask = mask;
        s->event_interval_ms = v3_int(p, "interval_ms", 100);
        cJSON *result = cJSON_CreateObject();
        v3_add_u32(result, "mask", mask);
        cJSON_AddStringToObject(result, "delivery", "best_effort_queryable");
        return result;
    }
    if (!strcmp(op, "control.acquire")) {
        if (c->owner >= 0 && c->owner != endpoint) {
            *error = "control_busy";
            return NULL;
        }
        c->owner = endpoint;
        snprintf(c->control, sizeof(c->control), "%s-c%08lx", c->boot, (unsigned long)++c->nonce);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "control", c->control);
        return r;
    }
    if (!strcmp(op, "control.release")) {
        c->owner = -1;
        c->control[0] = 0;
        return cJSON_CreateObject();
    }
    if (!strcmp(op, "capabilities")) {
        int page = v3_int(p, "page", 0);
        if (page < 0 || page >= v3_capability_pages()) {
            *error = "invalid_argument";
            return NULL;
        }
        return v3_capabilities(c, page);
    }
    if (!strcmp(op, "state"))
        return state_json(c, p, error);
    if (!strcmp(op, "mux.query")) {
        cJSON *r = state_json(c, p, error);
        if (!r)
            return NULL;
        cJSON *a = cJSON_DetachItemFromObjectCaseSensitive(r, "resources");
        v3_put(r, "routes", a);
        return r;
    }
    if (!strcmp(op, "resource.preflight") || !strcmp(op, "mux.preflight") ||
        !strcmp(op, "resource.open")) {
        if (!strcmp(op, "resource.open") && v3_exclusive_pending(c)) {
            *error = "maintenance_busy";
            return NULL;
        }
        v3_resource planned;
        *error = v3_plan(c, p, &planned);
        if (*error)
            return NULL;
        if (strcmp(op, "resource.open"))
            return v3_resource_json(&planned);
        v3_resource *r = NULL;
        for (int i = 0; i < V3_RESOURCES; ++i)
            if (c->resources[i].state == V_FREE) {
                r = &c->resources[i];
                break;
            }
        if (!r) {
            *error = "capacity";
            return NULL;
        }
        planned.definition = cJSON_Duplicate(p, true);
        if (!planned.definition) {
            *error = "capacity";
            return NULL;
        }
        planned.id = c->next_id++;
        planned.instance = planned.id;
        planned.epoch = c->epoch;
        planned.state = V_OPENING;
        *r = planned;
        v3_job *j = v3_schedule(c, r, "open", NULL);
        if (!j) {
            cJSON_Delete(r->definition);
            memset(r, 0, sizeof(*r));
            *error = "busy";
            return NULL;
        }
        ++c->revision;
        return job_json(j);
    }
    if (!strcmp(op, "resource.get")) {
        v3_resource *r = v3_find(c, v3_int(p, "resource", 0));
        if (!r) {
            *error = "resource_not_found";
            return NULL;
        }
        return v3_resource_json(r);
    }
    if (!strcmp(op, "resource.close") || !strcmp(op, "resource.recover") ||
        !strcmp(op, "resource.call")) {
        v3_resource *r = v3_find(c, v3_int(p, "resource", 0));
        if (!r) {
            *error = "resource_not_found";
            return NULL;
        }
        if (!v3_integer(v3_get(p, "instance"), 1, 2147483647) ||
            r->instance != (uint32_t)v3_int(p, "instance", 0)) {
            *error = "stale_instance";
            return NULL;
        }
        v3_resource *parent = v3_find(c, r->parent);
        bool readonly =
            !strcmp(op, "resource.call") && v3_action_readonly(r->kind, v3_str(p, "action", ""));
        if ((r->flow && !readonly) || r->busy || (parent && parent->busy)) {
            *error = "resource_busy";
            return NULL;
        }
        if (strcmp(op, "resource.call")) {
            if (children(c, r)) {
                if (!v3_bool(p, "cascade", false)) {
                    *error = "children_exist";
                    return NULL;
                }
                v3_close_tree(c, r);
                ++c->revision;
                cJSON *res = cJSON_CreateObject();
                cJSON_AddStringToObject(res, "state", "closing");
                return res;
            }
            r->state = V_CLOSING;
            v3_job *j = v3_schedule(c, r, "close", NULL);
            if (!j) {
                r->state = V_FAULT;
                *error = "busy";
                return NULL;
            }
            return job_json(j);
        }
        if (r->state != V_READY) {
            *error = "resource_not_ready";
            return NULL;
        }
        const char *action = v3_str(p, "action", "");
        if (!*action || strlen(action) >= 24) {
            *error = "invalid_argument";
            return NULL;
        }
        const cJSON *args = v3_get(p, "args");
        if (!cJSON_IsObject(args)) {
            *error = "invalid_argument";
            return NULL;
        }
        *error = v3_action_validate(r->kind, action, args);
        if (*error)
            return NULL;
        if (!strcmp(action, "open") || !strcmp(action, "close")) {
            *error = "invalid_argument";
            return NULL;
        }
        cJSON *owned = cJSON_Duplicate(args, true);
        if (!owned) {
            *error = "capacity";
            return NULL;
        }
        // Copy a buffer payload into the work before admission; later host writes cannot alter it.
        if (v3_get(owned, "buffer_id")) {
            v3_buffer *b = NULL;
            for (int i = 0; i < V3_BUFFERS; ++i)
                if (c->buffers[i].id && (int)c->buffers[i].id == v3_int(owned, "buffer_id", 0))
                    b = &c->buffers[i];
            if (!b) {
                cJSON_Delete(owned);
                *error = "buffer_not_found";
                return NULL;
            }
            char *encoded = v3_encode64(b->data, b->used);
            if (!encoded) {
                cJSON_Delete(owned);
                *error = "capacity";
                return NULL;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(owned, "buffer_id");
            cJSON_AddStringToObject(owned, "data", encoded);
            free(encoded);
        }
        v3_job *j = v3_schedule(c, r, action, owned);
        cJSON_Delete(owned);
        if (!j) {
            *error = "busy";
            return NULL;
        }
        return job_json(j);
    }
    if (!strcmp(op, "job.get")) {
        v3_job *j = v3_job_find(c, v3_int(p, "job_id", 0));
        if (!j) {
            *error = "job_expired";
            return NULL;
        }
        return job_json(j);
    }
    if (!strcmp(op, "job.cancel")) {
        if (!v3_job_cancel(c, v3_int(p, "job_id", 0))) {
            *error = "job_not_cancellable";
            return NULL;
        }
        return job_json(v3_job_find(c, v3_int(p, "job_id", 0)));
    }
    if (!strncmp(op, "buffer.", 7))
        return buffer_op(c, op, p, error);
    if (!strcmp(op, "outputs.stop")) {
        if (c->stopping) {
            *error = "stop_in_progress";
            return NULL;
        }
        ++c->epoch;
        ++c->revision;
        c->owner = -1;
        c->control[0] = 0;
        c->autostart_inhibited = true;
        c->stopping = true;
        v3_flow_stop_all(c);
        if (c->adapter.cancel)
            c->adapter.cancel(c->adapter.context, c->epoch);
        for (int i = 0; i < V3_RESOURCES; ++i)
            if (c->resources[i].state != V_FREE)
                v3_close_tree(c, &c->resources[i]);
        v3_job *j = new_job(c, "outputs.stop");
        if (!j) {
            *error = "busy";
            return NULL;
        }
        return job_json(j);
    }
    if (!strcmp(op, "diagnostics")) {
        cJSON *r = cJSON_CreateObject();
        v3_add_u32(r, "rejected", c->rejected);
        v3_add_u32(r, "stale_completions", c->stale);
        v3_add_u32(r, "lost_notifications", c->lost);
        cJSON_AddStringToObject(r, "timing", "software_monotonic");
        if (c->adapter.service) {
            cJSON *extra = c->adapter.service(c->adapter.context, op, p, error);
            if (extra)
                v3_put(r, "system", extra);
        }
        return r;
    }
    // System work follows the same owned payload and queryable completion contract.
    if (!strncmp(op, "storage.", 8) || !strncmp(op, "wifi.", 5) || !strncmp(op, "mqtt.", 5) ||
        !strncmp(op, "espnow.", 7) || !strncmp(op, "ota.", 4) || !strncmp(op, "power.", 6) ||
        !strncmp(op, "system.", 7) || !strncmp(op, "flow.", 5)) {
        if ((!strncmp(op, "mqtt.", 5) || !strncmp(op, "espnow.", 7) || !strncmp(op, "power.", 6)) &&
            strcmp(c->profile, "iot")) {
            *error = "feature_unavailable";
            return NULL;
        }
        if ((!strncmp(op, "ota.", 4) || !strncmp(op, "power.", 6) ||
             !strcmp(op, "storage.format") || !strcmp(op, "system.nvs_format")) &&
            !v3_idle(c)) {
            *error = "experiments_active";
            return NULL;
        }
        if (strlen(op) >= 24) {
            *error = "invalid_argument";
            return NULL;
        }
        cJSON *owned = NULL;
        if ((!strcmp(op, "mqtt.publish") || !strcmp(op, "espnow.send")) && v3_get(p, "buffer_id")) {
            if (v3_get(p, "data") || v3_get(p, "json")) {
                *error = "invalid_argument";
                return NULL;
            }
            v3_buffer *b = NULL;
            for (int i = 0; i < V3_BUFFERS; ++i)
                if (c->buffers[i].id && c->buffers[i].id == (uint32_t)v3_int(p, "buffer_id", 0))
                    b = &c->buffers[i];
            if (!b) {
                *error = "buffer_not_found";
                return NULL;
            }
            char *encoded = v3_encode64(b->data, b->used);
            owned = cJSON_Duplicate(p, true);
            if (!encoded || !owned) {
                free(encoded);
                cJSON_Delete(owned);
                *error = "capacity";
                return NULL;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(owned, "buffer_id");
            bool added = cJSON_AddStringToObject(owned, "data", encoded) != NULL;
            free(encoded);
            if (!added) {
                cJSON_Delete(owned);
                *error = "capacity";
                return NULL;
            }
        }
        v3_job *j = v3_schedule(c, NULL, op, owned ? owned : p);
        cJSON_Delete(owned);
        if (!j) {
            *error = "busy";
            return NULL;
        }
        return job_json(j);
    }
    *error = "unknown_operation";
    return NULL;
}
static bool observer(const char *op) {
    return !strcmp(op, "event.subscribe") || !strcmp(op, "event.unsubscribe") ||
           !strcmp(op, "gpio.group.read") || !strcmp(op, "session.ping") ||
           !strcmp(op, "control.acquire") || !strcmp(op, "capabilities") || !strcmp(op, "state") ||
           !strcmp(op, "mux.query") || !strcmp(op, "mux.preflight") ||
           !strcmp(op, "resource.preflight") || !strcmp(op, "resource.get") ||
           !strcmp(op, "flow.get") || !strcmp(op, "flow.list") || !strcmp(op, "job.get") ||
           !strcmp(op, "outputs.stop") || !strcmp(op, "diagnostics");
}
bool v3_readonly(v3_core *c, const char *op, const cJSON *p) {
    if (observer(op))
        return true;
    if (!strcmp(op, "resource.call")) {
        v3_resource *r = v3_find(c, v3_int(p, "resource", 0));
        return r && v3_action_readonly(r->kind, v3_str(p, "action", ""));
    }
    return false;
}
cJSON *v3_dispatch(v3_core *c, int endpoint, const char *op, const cJSON *p,
                   const char *control, const char **error) {
    *error = v3_operation_validate(op, p);
    if (*error)
        return NULL;
    bool observe = v3_readonly(c, op, p);
    if (!observe && (c->owner != endpoint || !c->control[0] || strcmp(c->control, control))) {
        *error = "control_required";
        return NULL;
    }
    if (c->stopping && !observe) {
        *error = "stop_in_progress";
        return NULL;
    }
    return operate(c, endpoint, op, p, error);
}
