#include "v3.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLOW_COUNT 2
#define FLOW_RESOURCES 12
#define FLOW_STEPS 64
#define FLOW_VARIABLES 8
typedef enum {
    F_LOADED,
    F_OPENING,
    F_RUNNING,
    F_PAUSED,
    F_STOPPING,
    F_STOPPED,
    F_COMPLETED,
    F_FAILED,
    F_SLEEPING
} flow_state;
typedef struct {
    uint32_t id, resources[FLOW_RESOURCES], waiting;
    char name[49];
    cJSON *definition, *last_result;
    flow_state state, resume;
    int pc, error, open_index, steps, nresources, nvars;
    const char *error_code;
    double variables[FLOW_VARIABLES];
    uint32_t loops[FLOW_STEPS];
    char fingerprint[17];
    bool restore_pending;
    int restore_pc;
    double restore_variables[FLOW_VARIABLES];
    uint32_t restore_loops[FLOW_STEPS];
    int64_t wake, deadline;
    bool awaiting;
} flow;
struct v3_flows {
    flow items[FLOW_COUNT];
};
static const char *const flow_states[] = {"loaded",  "opening",   "running", "paused",  "stopping",
                                          "stopped", "completed", "failed",  "sleeping"};
static bool identifier(const char *s, size_t max) {
    if (!*s || strlen(s) > max)
        return false;
    for (; *s; ++s)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.')
            return false;
    return true;
}
static int resource_index(const cJSON *def, const char *name) {
    const cJSON *a = v3_get(def, "resources");
    int i = 0;
    for (const cJSON *r = a ? a->child : NULL; r; r = r->next, ++i)
        if (!strcmp(v3_str(r, "name", ""), name))
            return i;
    return -1;
}
static int variable_index(const flow *f, const char *name) {
    const cJSON *a = v3_get(f->definition, "variables");
    int i = 0;
    for (const cJSON *v = a ? a->child : NULL; v; v = v->next, ++i)
        if (!strcmp(v->string, name))
            return i;
    return -1;
}
static const cJSON *step_at(const flow *f, int pc) {
    return cJSON_GetArrayItem(v3_get(f->definition, "steps"), pc);
}
static bool target(const cJSON *s, const char *k, int n) {
    const cJSON *v = v3_get(s, k);
    return !v || v3_integer(v, 0, n);
}
static bool cycle_visit(const flow *f, int pc, uint8_t marks[FLOW_STEPS]) {
    if (pc == f->steps)
        return true;
    if (marks[pc] == 1)
        return false;
    if (marks[pc] == 2)
        return true;
    const cJSON *s = step_at(f, pc);
    const char *op = v3_str(s, "op", "");
    // Waiting nodes break every possible CPU-only cycle. Finite loop back-edges are bounded.
    if (!strcmp(op, "delay") || !strcmp(op, "await") || !strcmp(op, "end") || !strcmp(op, "sleep"))
        return true;
    marks[pc] = 1;
    int next = pc + 1;
    if (!strcmp(op, "if")) {
        if (!cycle_visit(f, v3_int(s, "then", next), marks) ||
            !cycle_visit(f, v3_int(s, "else", next), marks))
            return false;
    } else if (!cycle_visit(f, next, marks))
        return false;
    if (v3_get(s, "on_error") && !cycle_visit(f, v3_int(s, "on_error", f->steps), marks))
        return false;
    marks[pc] = 2;
    return true;
}
static bool references(const flow *f, const cJSON *p) {
    if (cJSON_IsObject(p) && v3_get(p, "$result"))
        return v3_keys(p, "$result") && identifier(v3_str(p, "$result", ""), 32);
    if (cJSON_IsObject(p) && v3_get(p, "$var"))
        return v3_keys(p, "$var") && variable_index(f, v3_str(p, "$var", "")) >= 0;
    for (const cJSON *v = p ? p->child : NULL; v; v = v->next)
        if (!references(f, v))
            return false;
    return true;
}
static const char *validate(flow *f) {
    const cJSON *def = f->definition, *rs = v3_get(def, "resources"), *steps = v3_get(def, "steps"),
                *vars = v3_get(def, "variables");
    if (!v3_keys(def, "version profile autostart resources variables steps repeat_ms") ||
        v3_int(def, "version", 0) != 1 || !cJSON_IsArray(rs) || !cJSON_IsArray(steps) ||
        (vars && !cJSON_IsObject(vars)))
        return "invalid_flow";
    if (v3_get(def, "autostart") && !cJSON_IsBool(v3_get(def, "autostart")))
        return "invalid_flow";
    if (v3_get(def, "repeat_ms") && !v3_integer(v3_get(def, "repeat_ms"), 1, 86400000))
        return "invalid_repeat";
    f->steps = cJSON_GetArraySize(steps);
    f->nresources = cJSON_GetArraySize(rs);
    f->nvars = vars ? cJSON_GetArraySize(vars) : 0;
    if (f->steps < 1 || f->steps > FLOW_STEPS || f->nresources > FLOW_RESOURCES ||
        f->nvars > FLOW_VARIABLES)
        return "flow_capacity";
    for (const cJSON *r = rs->child; r; r = r->next) {
        const char *name = v3_str(r, "name", "");
        if (!identifier(name, 24) || !v3_keys(r, "name kind parent controller pins config"))
            return "invalid_flow_resource";
        for (const cJSON *b = r->next; b; b = b->next)
            if (!strcmp(name, v3_str(b, "name", "")))
                return "duplicate_flow_resource";
    }
    for (const cJSON *v = vars ? vars->child : NULL; v; v = v->next)
        if (!identifier(v->string, 24) || !cJSON_IsNumber(v) || !isfinite(v->valuedouble))
            return "invalid_variable";
    for (int i = 0; i < f->steps; ++i) {
        const cJSON *s = step_at(f, i);
        const char *op = v3_str(s, "op", "");
        if (!v3_keys(
                s, "op resource action args timeout_ms on_error store field poll_ms ms "
                   "variable value compare then else count target duration_ms wake_pin wake_level"))
            return "invalid_step";
        if (!target(s, "then", f->steps) || !target(s, "else", f->steps) ||
            !target(s, "on_error", f->steps))
            return "invalid_target";
        if (!strcmp(op, "call") || !strcmp(op, "await") || !strcmp(op, "service")) {
            if ((strcmp(op, "service") && resource_index(def, v3_str(s, "resource", "")) < 0) ||
                !*v3_str(s, "action", "") || strlen(v3_str(s, "action", "")) >= 24 ||
                !cJSON_IsObject(v3_get(s, "args")) ||
                !v3_integer(v3_get(s, "timeout_ms"), 1, 3600000))
                return "invalid_call";
            const char *a = v3_str(s, "action", "");
            if (!strcmp(op, "service") && strcmp(a, "mqtt.publish") && strcmp(a, "espnow.send") &&
                strcmp(a, "storage.record") && strcmp(a, "storage.append") &&
                strcmp(a, "system.time") && strcmp(a, "wifi.status") && strcmp(a, "mqtt.status"))
                return "invalid_service";
            if (!strcmp(a, "close") || !strcmp(a, "open"))
                return "invalid_call";
            if (!references(f, v3_get(s, "args")))
                return "invalid_variable";
            if (v3_get(s, "store") && variable_index(f, v3_str(s, "store", "")) < 0)
                return "invalid_variable";
            if (!strcmp(op, "await") &&
                (!*v3_str(s, "field", "") || !v3_integer(v3_get(s, "poll_ms"), 1, 60000)))
                return "invalid_wait";
        } else if (!strcmp(op, "sleep")) {
            if (!v3_integer(v3_get(s, "duration_ms"), 1, 86400000))
                return "invalid_sleep";
            if (v3_get(s, "wake_pin") && (!v3_integer(v3_get(s, "wake_pin"), 0, 5) ||
                                          !v3_pin_allowed(v3_int(s, "wake_pin", -1)) ||
                                          !v3_integer(v3_get(s, "wake_level"), 0, 1)))
                return "invalid_sleep";
        } else if (!strcmp(op, "delay")) {
            if (!v3_integer(v3_get(s, "ms"), 1, 86400000))
                return "invalid_delay";
        } else if (!strcmp(op, "set") || !strcmp(op, "add") || !strcmp(op, "if")) {
            if (variable_index(f, v3_str(s, "variable", "")) < 0 ||
                !cJSON_IsNumber(v3_get(s, "value")))
                return "invalid_variable";
            if (!strcmp(op, "if")) {
                const char *cmp = v3_str(s, "compare", "");
                if (strcmp(cmp, "eq") && strcmp(cmp, "ne") && strcmp(cmp, "lt") &&
                    strcmp(cmp, "le") && strcmp(cmp, "gt") && strcmp(cmp, "ge"))
                    return "invalid_compare";
            }
        } else if (!strcmp(op, "loop")) {
            if (!v3_integer(v3_get(s, "count"), 1, 100000) ||
                !v3_integer(v3_get(s, "target"), 0, i - 1))
                return "invalid_loop";
        } else if (strcmp(op, "end"))
            return "unknown_step";
    }
    uint8_t marks[FLOW_STEPS] = {0};
    for (int i = 0; i < f->steps; ++i)
        if (!cycle_visit(f, i, marks))
            return "busy_loop";
    return NULL;
}
const char *v3_flow_install(v3_core *c, const char *name, const cJSON *def, uint32_t *id) {
    if (!identifier(name, 48))
        return "invalid_flow_name";
    flow proposed = {.definition = (cJSON *)def};
    const char *error = validate(&proposed);
    if (error)
        return error;
    const char *profile = v3_str(def, "profile", "");
    if (strcmp(profile, c->profile))
        return "incompatible_profile";
    const cJSON *steps = v3_get(def, "steps");
    for (const cJSON *s = steps->child; s; s = s->next)
        if (!strcmp(v3_str(s, "op", ""), "sleep") && strcmp(c->profile, "iot"))
            return "feature_unavailable";
    for (const cJSON *s = steps->child; s; s = s->next)
        if (!strcmp(v3_str(s, "op", ""), "service")) {
            const char *action = v3_str(s, "action", "");
            if ((!strncmp(action, "mqtt.", 5) || !strncmp(action, "espnow.", 7)) &&
                strcmp(c->profile, "iot"))
                return "feature_unavailable";
        }
    if (!c->flows) {
        c->flows = calloc(1, sizeof(*c->flows));
        if (!c->flows)
            return "capacity";
    }
    flow *f = NULL;
    for (int i = 0; i < FLOW_COUNT; ++i)
        if (c->flows->items[i].id && !strcmp(c->flows->items[i].name, name)) {
            f = &c->flows->items[i];
            break;
        }
    if (f && (f->state == F_RUNNING || f->state == F_OPENING || f->state == F_PAUSED ||
              f->state == F_STOPPING || f->state == F_SLEEPING))
        return "flow_busy";
    if (!f)
        for (int i = 0; i < FLOW_COUNT; ++i)
            if (!c->flows->items[i].id) {
                f = &c->flows->items[i];
                break;
            }
    if (!f)
        return "flow_capacity";
    proposed.definition = cJSON_Duplicate(def, true);
    if (!proposed.definition)
        return "capacity";
    char *canonical = cJSON_PrintUnformatted(def);
    if (!canonical) {
        cJSON_Delete(proposed.definition);
        return "capacity";
    }
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)canonical; *p; ++p)
        hash = (hash ^ *p) * 1099511628211ULL;
    cJSON_free(canonical);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i)
        proposed.fingerprint[i] = hex[(hash >> (60 - 4 * i)) & 15];
    proposed.fingerprint[16] = 0;
    proposed.id = f->id ? f->id : c->next_id++;
    strcpy(proposed.name, name);
    proposed.state = F_LOADED;
    cJSON_Delete(f->definition);
    cJSON_Delete(f->last_result);
    *f = proposed;
    *id = f->id;
    return NULL;
}
static cJSON *flow_json(const flow *f) {
    cJSON *p = cJSON_CreateObject();
    v3_add_u32(p, "flow_id", f->id);
    cJSON_AddStringToObject(p, "name", f->name);
    cJSON_AddStringToObject(p, "state", flow_states[f->state]);
    cJSON_AddNumberToObject(p, "step", f->pc);
    cJSON_AddNumberToObject(p, "driver_error", f->error);
    if (f->error_code)
        cJSON_AddStringToObject(p, "error", f->error_code);
    cJSON_AddBoolToObject(p, "autostart", v3_bool(f->definition, "autostart", false));
    v3_add_u32(p, "waiting_job", f->waiting);
    cJSON *a = cJSON_AddArrayToObject(p, "resources");
    for (int i = 0; i < f->nresources; ++i)
        v3_push(a, cJSON_CreateNumber(f->resources[i]));
    cJSON *vars = cJSON_AddObjectToObject(p, "variables");
    int i = 0;
    const cJSON *v = v3_get(f->definition, "variables");
    for (const cJSON *pvar = v ? v->child : NULL; pvar; pvar = pvar->next, ++i)
        cJSON_AddNumberToObject(vars, pvar->string, f->variables[i]);
    return p;
}
static const char *start(v3_core *c, flow *f) {
    if (v3_exclusive_pending(c))
        return "maintenance_busy";
    if (f->state == F_RUNNING || f->state == F_OPENING || f->state == F_PAUSED ||
        f->state == F_STOPPING || f->state == F_SLEEPING)
        return "flow_busy";
    for (int i = 0; i < f->nresources; ++i)
        if (v3_find(c, f->resources[i]))
            return "resources_remaining";
    // Preflight on an isolated metadata registry, then commit all reservations together.
    v3_core *shadow = calloc(1, sizeof(*shadow));
    if (!shadow)
        return "capacity";
    memcpy(shadow->resources, c->resources, sizeof(c->resources));
    strcpy(shadow->profile, c->profile);
    shadow->next_id = c->next_id;
    v3_resource candidates[FLOW_RESOURCES];
    memset(candidates, 0, sizeof(candidates));
    int slots[FLOW_RESOURCES];
    const char *error = NULL;
    const cJSON *rs = v3_get(f->definition, "resources");
    for (int i = 0; i < f->nresources; ++i) {
        const cJSON *raw = cJSON_GetArrayItem(rs, i);
        cJSON *def = cJSON_Duplicate(raw, true);
        if (!def) {
            error = "capacity";
            break;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(def, "name");
        const cJSON *parent = v3_get(raw, "parent");
        if (parent) {
            int pi =
                resource_index(f->definition, cJSON_IsString(parent) ? parent->valuestring : "");
            if (pi < 0 || pi >= i) {
                cJSON_Delete(def);
                error = "invalid_flow_parent";
                break;
            }
            v3_replace(def, "parent", cJSON_CreateNumber(candidates[pi].id));
        }
        v3_resource r;
        error = v3_plan(shadow, def, &r);
        if (error) {
            cJSON_Delete(def);
            break;
        }
        int slot = -1;
        for (int j = 0; j < V3_RESOURCES; ++j)
            if (shadow->resources[j].state == V_FREE) {
                slot = j;
                break;
            }
        if (slot < 0) {
            cJSON_Delete(def);
            error = "capacity";
            break;
        }
        r.definition = def;
        r.id = shadow->next_id++;
        r.instance = r.id;
        r.epoch = c->epoch;
        r.flow = f->id;
        // Children see a valid parent while the complete reservation graph is checked.
        r.state = V_READY;
        r.flow = 0;
        shadow->resources[slot] = r;
        candidates[i] = r;
        slots[i] = slot;
    }
    if (error) {
        for (int i = 0; i < f->nresources; ++i)
            cJSON_Delete(candidates[i].definition);
        free(shadow);
        return error;
    }
    c->next_id = shadow->next_id;
    free(shadow);
    for (int i = 0; i < f->nresources; ++i) {
        candidates[i].state = V_OPENING;
        candidates[i].flow = f->id;
        c->resources[slots[i]] = candidates[i];
        f->resources[i] = candidates[i].id;
    }
    f->pc = f->open_index = f->error = 0;
    f->waiting = 0;
    f->wake = f->deadline = 0;
    f->awaiting = false;
    f->error_code = NULL;
    memset(f->loops, 0, sizeof(f->loops));
    int i = 0;
    const cJSON *v = v3_get(f->definition, "variables");
    for (const cJSON *var = v ? v->child : NULL; var; var = var->next, ++i)
        f->variables[i] = var->valuedouble;
    if (f->restore_pending) {
        f->pc = f->restore_pc;
        memcpy(f->variables, f->restore_variables, sizeof(f->variables));
        memcpy(f->loops, f->restore_loops, sizeof(f->loops));
        f->restore_pending = false;
    }
    cJSON_Delete(f->last_result);
    f->last_result = NULL;
    f->state = F_OPENING;
    c->autostart_inhibited = false;
    ++c->revision;
    if (c->adapter.service) {
        const char *ignored = NULL;
        cJSON *result = c->adapter.service(c->adapter.context, "flow.allow", NULL, &ignored);
        cJSON_Delete(result);
    }
    return NULL;
}
static void stop(v3_core *c, flow *f, bool failed) {
    if (f->state == F_SLEEPING && f->waiting) {
        c->autostart_inhibited = true;
        if (c->adapter.service) {
            const char *ignored = NULL;
            cJSON *p = c->adapter.service(c->adapter.context, "flow.inhibit", NULL, &ignored);
            cJSON_Delete(p);
        }
    }
    if (f->waiting)
        v3_job_cancel(c, f->waiting);
    for (int i = 0; i < f->nresources; ++i) {
        v3_resource *r = v3_find(c, f->resources[i]);
        if (r && r->busy)
            v3_job_cancel(c, r->busy);
        if (r)
            v3_close_tree(c, r);
    }
    f->state = failed ? F_FAILED : F_STOPPING;
    f->waiting = 0;
    f->awaiting = false;
    ++c->revision;
}
cJSON *v3_flow_operation(v3_core *c, const char *op, const cJSON *p, const char **error) {
    if (!strcmp(op, "flow.list")) {
        cJSON *out = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(out, "flows");
        if (c->flows)
            for (int i = 0; i < FLOW_COUNT; ++i)
                if (c->flows->items[i].id)
                    v3_push(a, flow_json(&c->flows->items[i]));
        return out;
    }
    flow *f = NULL;
    uint32_t id = v3_int(p, "flow_id", 0);
    if (c->flows)
        for (int i = 0; i < FLOW_COUNT; ++i)
            if (c->flows->items[i].id == id && id)
                f = &c->flows->items[i];
    if (!f) {
        *error = "flow_not_found";
        return NULL;
    }
    if (!strcmp(op, "flow.start")) {
        *error = start(c, f);
        if (*error)
            return NULL;
    } else if (!strcmp(op, "flow.pause")) {
        if (f->state != F_RUNNING && f->state != F_OPENING && f->state != F_SLEEPING) {
            *error = "flow_not_running";
            return NULL;
        }
        if (f->state == F_SLEEPING && f->waiting) {
            *error = "sleep_committed";
            return NULL;
        }
        f->resume = f->state;
        f->state = F_PAUSED;
        ++c->revision;
    } else if (!strcmp(op, "flow.resume")) {
        if (f->state != F_PAUSED) {
            *error = "flow_not_paused";
            return NULL;
        }
        f->state = f->resume;
        ++c->revision;
    } else if (!strcmp(op, "flow.stop"))
        stop(c, f, false);
    else if (!strcmp(op, "flow.remove")) {
        if (f->state == F_SLEEPING) {
            *error = "flow_busy";
            return NULL;
        }
        for (int i = 0; i < f->nresources; ++i)
            if (v3_find(c, f->resources[i])) {
                *error = "resources_remaining";
                return NULL;
            }
        cJSON_Delete(f->definition);
        cJSON_Delete(f->last_result);
        memset(f, 0, sizeof(*f));
        return cJSON_CreateObject();
    } else if (strcmp(op, "flow.get")) {
        *error = "unknown_operation";
        return NULL;
    }
    return flow_json(f);
}
static cJSON *resolve(const flow *f, const cJSON *p) {
    if (cJSON_IsObject(p) && v3_get(p, "$result")) {
        const cJSON *value = v3_get(f->last_result, v3_str(p, "$result", ""));
        return value ? cJSON_Duplicate(value, true) : NULL;
    }
    if (cJSON_IsObject(p) && v3_get(p, "$var")) {
        int i = variable_index(f, v3_str(p, "$var", ""));
        return i < 0 ? NULL : cJSON_CreateNumber(f->variables[i]);
    }
    cJSON *copy = cJSON_Duplicate(p, false);
    if (!copy)
        return NULL;
    for (const cJSON *v = p->child; v; v = v->next) {
        cJSON *child = resolve(f, v);
        if (!child) {
            cJSON_Delete(copy);
            return NULL;
        }
        if (cJSON_IsArray(p))
            v3_push(copy, child);
        else
            v3_put(copy, v->string, child);
    }
    return copy;
}
static bool satisfied(const cJSON *p) {
    return cJSON_IsTrue(p) || (cJSON_IsArray(p) && cJSON_GetArraySize(p) > 0) ||
           (cJSON_IsNumber(p) && p->valuedouble != 0);
}
static void fail_step(v3_core *c, flow *f, const cJSON *s, const char *code, int error) {
    if (f->waiting)
        v3_job_cancel(c, f->waiting);
    if (v3_get(s, "on_error")) {
        f->pc = v3_int(s, "on_error", f->steps);
        f->waiting = 0;
        f->deadline = 0;
        f->awaiting = false;
        return;
    }
    f->error = error;
    f->error_code = code;
    stop(c, f, true);
}
void v3_flow_tick(v3_core *c) {
    if (!c->flows || c->stopping)
        return;
    for (int fi = 0; fi < FLOW_COUNT; ++fi) {
        flow *f = &c->flows->items[fi];
        if (!f->id)
            continue;
        if (f->state == F_SLEEPING) {
            if (f->waiting) {
                v3_job *j = v3_job_find(c, f->waiting);
                if (!j || j->state != J_QUEUED) {
                    f->error_code = "sleep_failed";
                    f->state = F_FAILED;
                }
                continue;
            }
            bool remaining = false, failed = false;
            for (int i = 0; i < f->nresources; ++i) {
                v3_resource *r = v3_find(c, f->resources[i]);
                if (r) {
                    remaining = true;
                    if (r->state == V_FAULT)
                        failed = true;
                }
            }
            if (failed) {
                f->error_code = "sleep_cleanup_failed";
                f->state = F_FAILED;
                continue;
            }
            if (remaining || v3_exclusive_pending(c))
                continue;
            if (!v3_idle(c)) {
                f->error_code = "experiments_active";
                f->state = F_FAILED;
                continue;
            }
            const cJSON *step = step_at(f, f->pc);
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "mode", "deep");
            cJSON_AddNumberToObject(args, "duration_ms", v3_int(step, "duration_ms", 1));
            if (v3_get(step, "wake_pin")) {
                cJSON_AddNumberToObject(args, "wake_pin", v3_int(step, "wake_pin", 0));
                cJSON_AddNumberToObject(args, "wake_level", v3_int(step, "wake_level", 0));
            }
            cJSON *cp = cJSON_AddObjectToObject(args, "checkpoint");
            cJSON_AddStringToObject(cp, "name", f->name);
            cJSON_AddStringToObject(cp, "definition_id", f->fingerprint);
            cJSON_AddNumberToObject(cp, "pc", f->pc + 1);
            cJSON *vars = cJSON_AddArrayToObject(cp, "variables"),
                  *loops = cJSON_AddArrayToObject(cp, "loops");
            for (int i = 0; i < f->nvars; ++i)
                v3_push(vars, cJSON_CreateNumber(f->variables[i]));
            for (int i = 0; i < FLOW_STEPS; ++i)
                v3_push(loops, cJSON_CreateNumber(f->loops[i]));
            v3_job *job = v3_schedule(c, NULL, "power.sleep", args);
            cJSON_Delete(args);
            if (job)
                f->waiting = job->id;
            continue;
        }
        if (f->state == F_STOPPING) {
            bool left = false;
            for (int i = 0; i < f->nresources; ++i)
                if (v3_find(c, f->resources[i]))
                    left = true;
            if (!left)
                f->state = F_STOPPED;
            continue;
        }
        if (f->state == F_OPENING) {
            bool failed = false;
            for (int i = 0; i < f->nresources; ++i) {
                v3_resource *r = v3_find(c, f->resources[i]);
                if (!r || r->state == V_FAULT) {
                    failed = true;
                    break;
                }
            }
            if (failed) {
                f->error_code = "flow_open_failed";
                stop(c, f, true);
                continue;
            }
            if (f->open_index < f->nresources) {
                v3_resource *r = v3_find(c, f->resources[f->open_index]);
                if (r->state == V_READY)
                    ++f->open_index;
                else if (!r->busy)
                    v3_schedule(c, r, "open", NULL);
            } else
                f->state = F_RUNNING;
            continue;
        }
        if (f->state != F_RUNNING || c->now < f->wake)
            continue;
        if (f->pc >= f->steps) {
            int repeat = v3_int(f->definition, "repeat_ms", 0);
            if (repeat) {
                f->pc = 0;
                f->wake = c->now + repeat;
                memset(f->loops, 0, sizeof(f->loops));
            } else {
                f->state = F_COMPLETED;
                ++c->revision;
            }
            continue; // Completed flows retain their outputs until an explicit stop.
        }
        const cJSON *s = step_at(f, f->pc);
        const char *op = v3_str(s, "op", "");
        if (f->waiting) {
            v3_job *j = v3_job_find(c, f->waiting);
            if (!j) {
                fail_step(c, f, s, "job_expired", 0);
                continue;
            }
            if (j->state == J_QUEUED) {
                if (c->now >= f->deadline)
                    fail_step(c, f, s, "step_timeout", 0);
                continue;
            }
            f->waiting = 0;
            if (j->state != J_DONE) {
                fail_step(c, f, s, "transaction_failed", j->error);
                continue;
            }
            if (!strcmp(op, "await") && !satisfied(v3_get(j->result, v3_str(s, "field", "")))) {
                if (c->now >= f->deadline) {
                    fail_step(c, f, s, "wait_timeout", 0);
                    continue;
                }
                f->wake = c->now + v3_int(s, "poll_ms", 10);
                continue;
            }
            if (j->result_expired) {
                fail_step(c, f, s, "result_expired", 0);
                continue;
            }
            cJSON_Delete(f->last_result);
            f->last_result = j->result ? cJSON_Duplicate(j->result, true) : cJSON_CreateObject();
            if (v3_get(s, "store")) {
                int var = variable_index(f, v3_str(s, "store", ""));
                const cJSON *v = v3_get(j->result, v3_str(s, "field", ""));
                if (var < 0 || !cJSON_IsNumber(v)) {
                    fail_step(c, f, s, "invalid_result_field", 0);
                    continue;
                }
                f->variables[var] = v->valuedouble;
            }
            f->deadline = 0;
            f->awaiting = false;
            ++f->pc;
            ++c->revision;
            continue;
        }
        if (!strcmp(op, "call") || !strcmp(op, "await") || !strcmp(op, "service")) {
            if (!f->deadline)
                f->deadline = c->now + v3_int(s, "timeout_ms", 1000);
            if (c->now >= f->deadline) {
                fail_step(c, f, s, "admission_timeout", 0);
                continue;
            }
            int index = resource_index(f->definition, v3_str(s, "resource", ""));
            bool service = !strcmp(op, "service");
            v3_resource *r = service ? NULL : v3_find(c, f->resources[index]);
            if (!service && (!r || r->state != V_READY)) {
                fail_step(c, f, s, "resource_not_ready", 0);
                continue;
            }
            if (r && r->busy)
                continue;
            cJSON *args = resolve(f, v3_get(s, "args"));
            if (!args) {
                fail_step(c, f, s, "capacity", 0);
                continue;
            }
            if (!v3_get(args, "timeout_ms"))
                cJSON_AddNumberToObject(args, "timeout_ms",
                                        v3_int(s, "timeout_ms", 100) > V3_TIMEOUT_MAX
                                            ? V3_TIMEOUT_MAX
                                            : v3_int(s, "timeout_ms", 100));
            if (service)
                cJSON_DeleteItemFromObjectCaseSensitive(args, "timeout_ms");
            const char *invalid = service
                                      ? v3_operation_validate(v3_str(s, "action", ""), args)
                                      : v3_action_validate(r->kind, v3_str(s, "action", ""), args);
            if (invalid) {
                cJSON_Delete(args);
                fail_step(c, f, s, invalid, 0);
                continue;
            }
            v3_job *j = v3_schedule(c, r, v3_str(s, "action", ""), args);
            cJSON_Delete(args);
            if (j)
                f->waiting = j->id;
        } else if (!strcmp(op, "sleep")) {
            for (int i = 0; i < f->nresources; ++i) {
                v3_resource *r = v3_find(c, f->resources[i]);
                if (r)
                    v3_close_tree(c, r);
            }
            f->state = F_SLEEPING;
            ++c->revision;
        } else if (!strcmp(op, "delay")) {
            f->wake = c->now + v3_int(s, "ms", 1);
            ++f->pc;
        } else if (!strcmp(op, "set") || !strcmp(op, "add")) {
            int var = variable_index(f, v3_str(s, "variable", ""));
            double value = v3_num(s, "value", 0);
            if (!strcmp(op, "add"))
                value += f->variables[var];
            if (!isfinite(value)) {
                fail_step(c, f, s, "variable_overflow", 0);
                continue;
            }
            f->variables[var] = value;
            ++f->pc;
        } else if (!strcmp(op, "if")) {
            int var = variable_index(f, v3_str(s, "variable", ""));
            double x = f->variables[var], y = v3_num(s, "value", 0);
            const char *cmp = v3_str(s, "compare", "");
            bool yes = !strcmp(cmp, "eq")   ? x == y
                       : !strcmp(cmp, "ne") ? x != y
                       : !strcmp(cmp, "lt") ? x < y
                       : !strcmp(cmp, "le") ? x <= y
                       : !strcmp(cmp, "gt") ? x > y
                                            : x >= y;
            f->pc = v3_int(s, yes ? "then" : "else", f->pc + 1);
        } else if (!strcmp(op, "loop")) {
            if (++f->loops[f->pc] < (uint32_t)v3_int(s, "count", 1))
                f->pc = v3_int(s, "target", 0);
            else {
                f->loops[f->pc] = 0;
                ++f->pc;
            }
        } else
            f->pc = f->steps;
    }
}
void v3_flow_stop_all(v3_core *c) {
    if (c->flows)
        for (int i = 0; i < FLOW_COUNT; ++i)
            if (c->flows->items[i].id)
                stop(c, &c->flows->items[i], false);
}
void v3_flow_boot_error(v3_core *c, uint32_t id, const char *error) {
    if (c->flows)
        for (int i = 0; i < FLOW_COUNT; ++i)
            if (c->flows->items[i].id == id) {
                c->flows->items[i].state = F_FAILED;
                c->flows->items[i].error_code = error;
            }
}
const char *v3_flow_restore(v3_core *c, uint32_t id, const cJSON *cp) {
    flow *f = NULL;
    if (c->flows)
        for (int i = 0; i < FLOW_COUNT; ++i)
            if (c->flows->items[i].id == id)
                f = &c->flows->items[i];
    if (!f || !v3_keys(cp, "name definition_id pc variables loops"))
        return "invalid_checkpoint";
    if (strcmp(v3_str(cp, "name", ""), f->name) ||
        strcmp(v3_str(cp, "definition_id", ""), f->fingerprint))
        return "checkpoint_definition_changed";
    if (!v3_integer(v3_get(cp, "pc"), 0, f->steps))
        return "invalid_checkpoint";
    const cJSON *vars = v3_get(cp, "variables"), *loops = v3_get(cp, "loops");
    if (!cJSON_IsArray(vars) || cJSON_GetArraySize(vars) != f->nvars || !cJSON_IsArray(loops) ||
        cJSON_GetArraySize(loops) != FLOW_STEPS)
        return "invalid_checkpoint";
    for (int i = 0; i < f->nvars; ++i) {
        const cJSON *v = cJSON_GetArrayItem(vars, i);
        if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble))
            return "invalid_checkpoint";
        f->restore_variables[i] = v->valuedouble;
    }
    for (int i = 0; i < FLOW_STEPS; ++i) {
        const cJSON *v = cJSON_GetArrayItem(loops, i);
        if (!v3_integer(v, 0, 100000))
            return "invalid_checkpoint";
        f->restore_loops[i] = v->valueint;
    }
    f->restore_pc = v3_int(cp, "pc", 0);
    f->restore_pending = true;
    return NULL;
}
void v3_flow_destroy(v3_core *c) {
    if (c->flows) {
        for (int i = 0; i < FLOW_COUNT; ++i) {
            cJSON_Delete(c->flows->items[i].definition);
            cJSON_Delete(c->flows->items[i].last_result);
        }
        free(c->flows);
        c->flows = NULL;
    }
}
