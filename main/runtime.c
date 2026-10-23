#include "limivia.h"
#include "peripherals.h"
#include "services.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

typedef struct {
    int endpoint, priority;
    uint32_t generation;
    size_t size;
    bool retained;
    char text[];
} request;
typedef struct {
    int endpoint, priority;
    uint32_t generation;
    size_t size;
    char *text;
} response;
typedef struct {
    v3_work *work;
    int error;
    bool external;
    cJSON *result;
} completion;
static v3_core core;
static QueueHandle_t requests[3], responses[3], notices[3], work_queues[9], completed;
static atomic_uint occupied[V3_ENDPOINTS], admission_lost, tx_lost;
static atomic_uint notice_failed;
static atomic_size_t tx_bytes;
static TaskHandle_t senders[3];
static char encoding[512]; // actor-owned small-frame scratch; large discovery pages are transient.
static uint32_t stream_seen[V3_ENDPOINTS][V3_RESOURCES],
    stream_instances[V3_ENDPOINTS][V3_RESOURCES], notified_revision;
static int64_t notify_at[V3_ENDPOINTS];
static unsigned stream_cursor[V3_ENDPOINTS];
static TaskHandle_t actor;
static atomic_bool initialized;
static uint32_t autostart[2];
static bool boot_started;
static unsigned boot_failures;
static const char *boot_error;
static TaskHandle_t workers[9];
static atomic_bool worker_idle[8];
static void worker(void *);
static char *owned_frame(const cJSON *json, size_t *size) {
    *size = v3_frame(json, encoding, sizeof(encoding));
    if (!*size) {
        char *large = malloc(LV_RESPONSE_MAX + 8);
        if (!large)
            return NULL;
        *size = v3_frame(json, large, LV_RESPONSE_MAX + 8);
        if (!*size) {
            free(large);
            return NULL;
        }
        char *text = malloc(*size + 1);
        if (text)
            memcpy(text, large, *size + 1);
        free(large);
        return text;
    }
    char *text = malloc(*size + 1);
    if (text)
        memcpy(text, encoding, *size + 1);
    return text;
}
static void emit(int target, unsigned mask, const char *event, uint32_t resource, uint32_t job,
                 uint32_t sequence, uint32_t dropped) {
    for (int i = 0; i < V3_ENDPOINTS; ++i) {
        if (target >= 0 && target != i)
            continue;
        v3_session *s = &core.sessions[i];
        if (!s->token[0] || !(s->event_mask & mask))
            continue;
        unsigned allocation_start = v3_json_failures();
        cJSON *p = cJSON_CreateObject();
        v3_add_u32(p, "event_sequence", ++s->event_sequence);
        v3_add_u32(p, "revision", core.revision);
        v3_add_u32(p, "resource", resource);
        v3_add_u32(p, "job_id", job);
        v3_add_u32(p, "sequence", sequence);
        v3_add_u32(p, "dropped", dropped);
        v3_add_u32(p, "instance", resource);
        p = v3_rpc_notice(&core, i, event, p);
        response out = {.endpoint = i, .priority = -1, .generation = transport_generation(i)};
        out.text = allocation_start == v3_json_failures() ? owned_frame(p, &out.size) : NULL;
        cJSON_Delete(p);
        int lane = i < 2 ? i : 2;
        if (!out.text || atomic_load(&tx_bytes) + out.size > 24576 ||
            xQueueSend(notices[lane], &out, 0) != pdTRUE) {
            free(out.text);
            ++core.lost;
        } else {
            atomic_fetch_add(&tx_bytes, out.size);
            xTaskNotifyGive(senders[lane]);
        }
    }
}
static bool enqueue(void *ctx, v3_work *w) {
    (void)ctx;
    int lane = w->slot < 0 ? 8 : v3_lane(w->kind, w->controller);
    if (!workers[lane] &&
        xTaskCreate(worker, "lv_worker", 4096, (void *)(intptr_t)lane, 5, &workers[lane]) != pdPASS)
        return false;
    return xQueueSend(work_queues[lane], &w, 0) == pdTRUE;
}
static void cancel(void *ctx, uint32_t epoch) {
    (void)ctx;
    atomic_store(&v3_epoch, epoch);
    v3_inhibit_latch(true);
    atomic_store(&v3_inhibit_value, true);
    atomic_store(&v3_inhibit_requested, true);
}
static cJSON *service(void *ctx, const char *op, const cJSON *params, const char **error) {
    (void)ctx;
    (void)params;
    (void)error;
    if (!strcmp(op, "stop.persisted")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(
            p, "done", !atomic_load(&v3_inhibit_requested) && !atomic_load(&v3_inhibit_busy));
        cJSON_AddNumberToObject(p, "error", atomic_load(&v3_inhibit_error));
        return p;
    }
    if (!strcmp(op, "flow.allow")) {
        v3_inhibit_latch(false);
        atomic_store(&v3_inhibit_value, false);
        atomic_store(&v3_inhibit_requested, true);
    }
    if (!strcmp(op, "flow.inhibit")) {
        v3_inhibit_latch(true);
        atomic_store(&v3_inhibit_value, true);
        atomic_store(&v3_inhibit_requested, true);
    }
    if (!strcmp(op, "diagnostics")) {
        cJSON *p = v3_system_status(), *a = cJSON_AddArrayToObject(p, "worker_stack_free_bytes");
        for (int i = 0; i < 9; ++i)
            v3_push(a,
                    cJSON_CreateNumber(workers[i] ? uxTaskGetStackHighWaterMark(workers[i]) : 0));
        cJSON_AddNumberToObject(p, "retained_result_bytes", core.retained_result_bytes);
        cJSON_AddNumberToObject(p, "actor_stack_free_bytes", uxTaskGetStackHighWaterMark(NULL));
        cJSON_AddNumberToObject(p, "boot_flow_failures", boot_failures);
        if (boot_error)
            cJSON_AddStringToObject(p, "boot_flow_error", boot_error);
        cJSON_AddNumberToObject(p, "tx_queued_bytes", atomic_load(&tx_bytes));
        cJSON_AddNumberToObject(p, "admission_rejected", atomic_load(&admission_lost));
        cJSON_AddNumberToObject(p, "tx_failed", atomic_load(&tx_lost));
        return p;
    }
    return NULL;
}
void control_wake(void) {
    if (actor)
        xTaskNotifyGive(actor);
}
void control_submit(lv_source endpoint, uint32_t generation, const char *text, size_t n) {
    control_submit_message(endpoint, generation, text, n, false);
}
void control_submit_message(lv_source endpoint, uint32_t generation, const char *text, size_t n,
                            bool retained) {
    if (!atomic_load(&initialized) || endpoint < 0 || endpoint >= V3_ENDPOINTS ||
        n > LV_REQUEST_MAX)
        return;
    int priority = v3_rpc_priority(text, n);
    if (priority < 0) {
        atomic_fetch_add(&admission_lost, 1);
        return;
    }
    unsigned bit = 1u << priority;
    if (atomic_fetch_or(&occupied[endpoint], bit) & bit) {
        atomic_fetch_add(&admission_lost, 1);
        return;
    }
    request *r = malloc(sizeof(*r) + n + 1);
    if (!r) {
        atomic_fetch_and(&occupied[endpoint], ~bit);
        atomic_fetch_add(&admission_lost, 1);
        return;
    }
    *r = (request){.endpoint = endpoint,
                   .priority = priority,
                   .generation = generation,
                   .size = n,
                   .retained = retained};
    memcpy(r->text, text, n);
    r->text[n] = 0;
    if (xQueueSend(requests[priority], &r, 0) != pdTRUE) {
        free(r);
        atomic_fetch_and(&occupied[endpoint], ~bit);
        atomic_fetch_add(&admission_lost, 1);
        return;
    }
    control_wake();
}
static void worker(void *arg) {
    int lane = (intptr_t)arg;
    v3_work *w;
    for (;;) {
        if (xQueueReceive(work_queues[lane], &w, pdMS_TO_TICKS(5)) == pdTRUE) {
            completion done = {.work = w};
            unsigned allocation_start = v3_json_failures();
            if (v3_cancelled(w) && strcmp(w->action, "close"))
                done.error = ESP_ERR_INVALID_STATE;
            else
                done.error = w->slot < 0 ? v3_system_work(w, &done.result, &done.external)
                                         : v3_peripheral(w, &done.result, &done.external);
            if (!done.error && allocation_start != v3_json_failures())
                done.error = ESP_ERR_NO_MEM;
            if (done.error) {
                if (!done.result)
                    done.result = cJSON_CreateObject();
                const char *code = done.error == ESP_ERR_NO_MEM          ? "capacity"
                                   : done.error == ESP_ERR_TIMEOUT       ? "timeout"
                                   : done.error == ESP_ERR_INVALID_ARG   ? "invalid_argument"
                                   : done.error == ESP_ERR_NOT_SUPPORTED ? "unsupported_operation"
                                   : done.error == ESP_ERR_INVALID_RESPONSE && w->slot >= 0 &&
                                           (w->kind == V_I2C || w->kind == V_I2C_DEVICE)
                                       ? "nack"
                                   : done.error == ESP_ERR_INVALID_STATE ? "invalid_state"
                                                                         : "driver_error";
                cJSON_AddStringToObject(done.result, "error_code", code);
            }
            xQueueSend(completed, &done, portMAX_DELAY);
            control_wake();
        }
        if (lane == 8)
            v3_services_poll();
        else
            v3_poll_lane(lane);
        if (lane < 8 && !atomic_load(&v3_lane_slots[lane]) &&
            !uxQueueMessagesWaiting(work_queues[lane])) {
            atomic_store(&worker_idle[lane], true);
            control_wake();
            if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 2)
                vTaskDelete(NULL);
        }
    }
}
static void sender(void *arg) {
    int lane = (intptr_t)arg;
    response r;
    for (;;) {
        if (xQueueReceive(responses[lane], &r, 0) != pdTRUE &&
            xQueueReceive(notices[lane], &r, 0) != pdTRUE) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (r.priority >= 0)
            atomic_fetch_and(&occupied[r.endpoint], ~(1u << r.priority));
        if (r.generation != transport_generation(r.endpoint) ||
            !transport_send(r.endpoint, r.generation, r.text, r.size)) {
            atomic_fetch_add(&tx_lost, 1);
            if (r.priority < 0)
                atomic_fetch_add(&notice_failed, 1);
        }
        free(r.text);
        atomic_fetch_sub(&tx_bytes, r.size);
    }
}
static void process_request(request *r) {
    if (r->generation != transport_generation(r->endpoint)) {
        atomic_fetch_and(&occupied[r->endpoint], ~(1u << r->priority));
        free(r);
        return;
    }
    size_t n = 0;
    char *text = v3_rpc_alloc(&core, r->endpoint, r->generation, r->text, r->size,
                               esp_timer_get_time() / 1000, r->retained, &n);
    if (!text || !n || n == SIZE_MAX) {
        if (n == SIZE_MAX)
            atomic_fetch_add(&tx_lost, 1);
        free(text);
        atomic_fetch_and(&occupied[r->endpoint], ~(1u << r->priority));
        free(r);
        return;
    }
    char *small = realloc(text, n + 1);
    if (small)
        text = small;
    response out = {.endpoint = r->endpoint, .priority = r->priority,
                    .generation = r->generation, .text = text, .size = n};
    int lane = r->endpoint < 2 ? r->endpoint : 2;
    size_t limit = r->priority == 2 ? 32768 : 24576;
    if (atomic_load(&tx_bytes) + n > limit) {
        free(text);
        atomic_fetch_add(&tx_lost, 1);
        atomic_fetch_and(&occupied[r->endpoint], ~(1u << r->priority));
    } else {
        atomic_fetch_add(&tx_bytes, n);
        if (xQueueSend(responses[lane], &out, 0) != pdTRUE) {
            free(text);
            atomic_fetch_sub(&tx_bytes, n);
            atomic_fetch_add(&tx_lost, 1);
            atomic_fetch_and(&occupied[r->endpoint], ~(1u << r->priority));
        } else
            xTaskNotifyGive(senders[lane]);
    }
    free(r);
}
static void interrupt_batch(void *ctx) {
    (void)ctx;
    static bool servicing_stop;
    // RX/host tasks run below the actor: allow them to admit newly arrived stops.
    vTaskDelay(1);
    if (!servicing_stop) {
        request *urgent = NULL;
        if (xQueueReceive(requests[2], &urgent, 0) == pdTRUE) {
            servicing_stop = true;
            process_request(urgent);
            servicing_stop = false;
        }
    }
    v3_tick(&core, esp_timer_get_time() / 1000);
    for (int i = 0; i < V3_ENDPOINTS; ++i)
        v3_disconnect(&core, i, transport_generation(i));
}
static void execute_task(void *arg) {
    (void)arg;
    for (;;) {
        completion done;
        for (int i = 0; i < V3_JOBS && xQueueReceive(completed, &done, 0) == pdTRUE; ++i) {
            v3_complete(&core, done.work, done.error, done.external, done.result);
            emit(-1, 1, "job.finished", done.work->resource, done.work->job, 0, 0);
            v3_work_free(done.work);
        }
        v3_tick(&core, esp_timer_get_time() / 1000);
        for (int lane = 0; lane < 8; ++lane)
            if (atomic_exchange(&worker_idle[lane], false)) {
                bool live = false;
                for (int slot = 0; slot < V3_RESOURCES; ++slot) {
                    v3_resource *r = &core.resources[slot];
                    if (r->state != V_FREE && v3_lane(r->kind, r->controller) == lane) {
                        live = true;
                        break;
                    }
                }
                TaskHandle_t task = workers[lane];
                if (task) {
                    if (!live && !uxQueueMessagesWaiting(work_queues[lane])) {
                        workers[lane] = NULL;
                        xTaskNotify(task, 2, eSetValueWithOverwrite);
                    } else
                        xTaskNotify(task, 1, eSetValueWithOverwrite);
                }
            }
        if (!boot_started && atomic_load(&v3_boot_healthy)) {
            boot_started = true;
            if (!core.autostart_inhibited)
                for (int i = 0; i < 2; ++i)
                    if (autostart[i]) {
                        cJSON *params = cJSON_CreateObject();
                        v3_add_u32(params, "flow_id", autostart[i]);
                        const char *error = NULL;
                        cJSON *result = v3_flow_operation(&core, "flow.start", params, &error);
                        if (error && strcmp(error, "flow_busy")) {
                            v3_flow_boot_error(&core, autostart[i], error);
                            ++boot_failures;
                            boot_error = error;
                        }
                        cJSON_Delete(result);
                        cJSON_Delete(params);
                    }
        }
        for (int i = 0; i < V3_ENDPOINTS; ++i)
            v3_disconnect(&core, i, transport_generation(i));
        request *r = NULL;
        for (int p = 2; p >= 0; --p)
            if (xQueueReceive(requests[p], &r, 0) == pdTRUE)
                break;
        if (r)
            process_request(r);
        core.lost += atomic_exchange(&notice_failed, 0);
        if (core.revision != notified_revision) {
            emit(-1, 6, "state.changed", 0, 0, 0, 0);
            notified_revision = core.revision;
        }
        for (int endpoint = 0; endpoint < V3_ENDPOINTS; ++endpoint) {
            v3_session *s = &core.sessions[endpoint];
            if (!s->token[0] || !(s->event_mask & 8) || core.now < notify_at[endpoint])
                continue;
            notify_at[endpoint] = core.now + s->event_interval_ms;
            for (int k = 0; k < V3_RESOURCES; ++k) {
                int slot = (stream_cursor[endpoint] + k) % V3_RESOURCES;
                v3_resource *resource = &core.resources[slot];
                v3_stream_status *p = &v3_streams[slot];
                if (resource->state != V_READY || atomic_load(&p->instance) != resource->instance)
                    continue;
                if (stream_instances[endpoint][slot] != resource->instance) {
                    stream_instances[endpoint][slot] = resource->instance;
                    stream_seen[endpoint][slot] = 0;
                }
                uint32_t sequence = atomic_load(&p->sequence);
                if (sequence == stream_seen[endpoint][slot])
                    continue;
                stream_seen[endpoint][slot] = sequence;
                emit(endpoint, 8, "data.available", resource->id, 0, sequence,
                     atomic_load(&p->dropped));
                stream_cursor[endpoint] = (slot + 1) % V3_RESOURCES;
                break;
            }
        }
        bool active = false;
        for (int i = 0; i < V3_ENDPOINTS; ++i)
            if (core.sessions[i].token[0])
                active = true;
        gpio_set_level(12, active);
        if (!r)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    }
}
void control_init(void) {
    for (int pin = 0; pin < 22; ++pin)
        if (v3_pin_allowed(pin))
            ESP_ERROR_CHECK(gpio_reset_pin(pin));
    for (int i = 0; i < 3; ++i) {
        requests[i] = xQueueCreate(V3_ENDPOINTS, sizeof(request *));
        responses[i] = xQueueCreate(i < 2 ? 3 : 8, sizeof(response));
        notices[i] = xQueueCreate(1, sizeof(response));
        configASSERT(requests[i] && responses[i] && notices[i]);
    }
    for (int i = 0; i < 9; ++i) {
        work_queues[i] = xQueueCreate(2, sizeof(v3_work *));
        configASSERT(work_queues[i]);
    }
    completed = xQueueCreate(V3_JOBS, sizeof(completion));
    configASSERT(completed);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device[18], boot[17], build[65];
    snprintf(device, sizeof(device), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    snprintf(boot, sizeof(boot), "%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());
    const esp_app_desc_t *description = esp_app_get_description();
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        build[2 * i] = hex[description->app_elf_sha256[i] >> 4];
        build[2 * i + 1] = hex[description->app_elf_sha256[i] & 15];
    }
    build[64] = 0;
    v3_init(&core, (v3_adapter){.enqueue = enqueue, .cancel = cancel, .service = service,
                                       .interrupt = interrupt_batch}, boot,
            device, build, LV_PROFILE);
    core.autostart_inhibited = v3_autostart_inhibited();
    if (v3_storage_ready()) {
        int n = 0;
        cJSON *cp = v3_checkpoint_take();
        const char *resume_name = v3_str(cp, "name", "");
        if (cp && !core.autostart_inhibited) {
            cJSON *def = NULL;
            uint32_t id = 0;
            const char *failure = "checkpoint_file_unavailable";
            if (v3_storage_load(resume_name, &def) == ESP_OK) {
                failure = v3_flow_install(&core, resume_name, def, &id);
                if (!failure)
                    failure = v3_flow_restore(&core, id, cp);
            }
            cJSON_Delete(def);
            if (failure) {
                ++boot_failures;
                boot_error = failure;
                if (id)
                    v3_flow_boot_error(&core, id, failure);
            } else
                autostart[n++] = id;
        }
        DIR *dir = opendir("/data");
        struct dirent *entry;
        while (dir && (entry = readdir(dir))) {
            size_t length = strlen(entry->d_name);
            if (length < 10 || strcmp(entry->d_name + length - 10, ".flow.json"))
                continue;
            if (cp && !core.autostart_inhibited && !strcmp(entry->d_name, resume_name))
                continue;
            cJSON *def = NULL;
            uint32_t id = 0;
            if (v3_storage_load(entry->d_name, &def) == ESP_OK) {
                if (v3_bool(def, "autostart", false)) {
                    const char *failure = n >= 2 ? "autostart_capacity"
                                                 : v3_flow_install(&core, entry->d_name, def, &id);
                    if (failure) {
                        ++boot_failures;
                        boot_error = failure;
                    } else
                        autostart[n++] = id;
                }
            }
            cJSON_Delete(def);
        }
        if (dir)
            closedir(dir);
        cJSON_Delete(cp);
    }
    gpio_config_t led = {.pin_bit_mask = 1ULL << 12, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&led));
    configASSERT(xTaskCreate(worker, "lv_system", 6144, (void *)(intptr_t)8, 5, &workers[8]) ==
                 pdPASS);
    for (int i = 0; i < 3; ++i)
        configASSERT(xTaskCreate(sender, "lv_tx", 4096, (void *)(intptr_t)i, 4, &senders[i]) ==
                     pdPASS);
    configASSERT(xTaskCreate(execute_task, "lv_actor", 8192, NULL, 6, &actor) == pdPASS);
    atomic_store(&initialized, true);
}
