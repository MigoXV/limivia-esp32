#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define LV_PROTOCOL 3
#define LV_RPC_BATCH_MAX 8
#define LV_RPC_CONTRACT "limivia-v3-jsonrpc2"
#define LV_REQUEST_MAX 1024
#define LV_RESPONSE_MAX 8192
#define V3_RESOURCES 24
#define V3_JOBS 16
#define V3_BUFFERS 4
#define V3_BUFFER_MAX 4096
#define V3_TOKEN 34
#define V3_ENDPOINTS 8
#define V3_TIMEOUT_MAX 1000
#ifndef LV_PROFILE
#define LV_PROFILE "bus"
#endif
typedef enum {
    LV_USB,
    LV_BLE,
    LV_WS0,
    LV_WS1,
    LV_MQTT0,
    LV_MQTT1,
    LV_MQTT2,
    LV_MQTT3,
    LV_SOURCES
} lv_source;
typedef struct {
    char text[LV_REQUEST_MAX + 1];
    size_t used;
    bool dropping;
    int64_t last;
} lv_rx;
typedef void (*lv_frame_fn)(void *, const char *, size_t);
void lv_receive(lv_rx *, const uint8_t *, size_t, int64_t, lv_frame_fn, void *);

typedef enum {
    V_GPIO,
    V_PWM,
    V_ADC,
    V_ADC_CONT,
    V_UART,
    V_I2C,
    V_I2C_DEVICE,
    V_I2C_SLAVE,
    V_SPI,
    V_SPI_DEVICE,
    V_SPI_SLAVE,
    V_TWAI,
    V_RMT_TX,
    V_RMT_RX,
    V_I2S,
    V_SDM,
    V_TIMER,
    V_TEMPERATURE,
    V_KIND_COUNT
} v3_kind;
typedef enum { V_FREE, V_OPENING, V_READY, V_CLOSING, V_FAULT } v3_state;
typedef struct {
    uint32_t id, instance, parent, pins, epoch, busy;
    v3_kind kind;
    v3_state state;
    int controller, channel, timer, error, cleanup_error;
    uint32_t flow;
    cJSON *definition; // owned until confirmed close; never borrowed from a request
} v3_resource;
typedef enum { J_FREE, J_QUEUED, J_DONE, J_FAILED, J_CANCELLED } v3_job_state;
typedef struct {
    uint32_t id, epoch, resource, instance;
    v3_job_state state;
    int error;
    bool external_effect;
    bool result_expired;
    size_t result_bytes;
    struct v3_work *work;
    char action[24];
    cJSON *result;
} v3_job;
typedef struct {
    uint32_t id;
    size_t size, used;
    uint8_t *data;
} v3_buffer;
typedef struct {
    char token[V3_TOKEN];
    uint32_t generation;
    uint32_t event_mask, event_sequence, event_interval_ms;
    uint32_t sequence_high;
    uint64_t sequence_seen;
    char client_nonce[33];
    int64_t expires;
} v3_session;
typedef struct v3_work {
    uint32_t job, epoch, resource, instance;
    atomic_bool cancelled;
    int slot, parent_slot, controller, channel, timer;
    v3_kind kind;
    char action[24];
    cJSON *definition, *params; // work owns both; no actor pointers cross threads
} v3_work;
typedef struct v3_core v3_core;
typedef struct v3_flows v3_flows;
typedef struct {
    void *context;
    bool (*enqueue)(void *, v3_work *);     // takes ownership on true
    void (*interrupt)(void *); // actor-only: service urgent work between batch items
    void (*cancel)(void *, uint32_t epoch); // nonblocking epoch publication
    cJSON *(*service)(void *, const char *, const cJSON *, const char **error);
} v3_adapter;
struct v3_core {
    char boot[17], device[18], build[65], profile[8];
    uint32_t next_id, revision, epoch, nonce, rejected, stale, lost;
    size_t retained_result_bytes;
    int owner;
    char control[V3_TOKEN];
    bool stopping, autostart_inhibited;
    int64_t now;
    v3_resource resources[V3_RESOURCES];
    v3_job jobs[V3_JOBS];
    v3_buffer buffers[V3_BUFFERS];
    v3_session sessions[V3_ENDPOINTS];
    v3_adapter adapter;
    v3_flows *flows;
};
extern const char *const v3_kinds[V_KIND_COUNT];
extern const char *const v3_states[];
void v3_init(v3_core *, v3_adapter, const char *boot, const char *device, const char *build,
             const char *profile);
void v3_destroy(v3_core *);
cJSON *v3_handle(v3_core *, int endpoint, uint32_t generation, const char *, size_t, int64_t);
cJSON *v3_handle_message(v3_core *, int, uint32_t, const char *, size_t, int64_t, bool retained);
// Wire codec: returns complete JSON+LF, or zero for notifications/admission rejection.
size_t v3_rpc_wire(v3_core *, int, uint32_t, const char *, size_t, int64_t, bool,
                   char *, size_t);
// malloc-owned frame, starting at 512 bytes; SIZE_MAX size signals allocation loss.
char *v3_rpc_alloc(v3_core *, int, uint32_t, const char *, size_t, int64_t, bool, size_t *);
int v3_rpc_priority(const char *, size_t);
cJSON *v3_rpc_notice(const v3_core *, int, const char *, cJSON *); // takes args
bool v3_readonly(v3_core *, const char *, const cJSON *);
cJSON *v3_dispatch(v3_core *, int, const char *, const cJSON *, const char *, const char **);
int v3_capability_pages(void);
cJSON *v3_operations_json(const char *profile, int page);
void v3_tick(v3_core *, int64_t);
void v3_disconnect(v3_core *, int endpoint, uint32_t generation);
void v3_complete(v3_core *, v3_work *, int error, bool external_effect, cJSON *result);
void v3_work_free(v3_work *);
bool v3_idle(const v3_core *);
v3_resource *v3_find(v3_core *, uint32_t id);
const char *v3_plan(v3_core *, const cJSON *, v3_resource *);
cJSON *v3_capabilities(const v3_core *, int page);
cJSON *v3_resource_json(const v3_resource *);
bool v3_available(const char *profile, v3_kind);
bool v3_pin_allowed(int);
const char *v3_pin_reserved(int);

// Strict JSON and binary codec shared with the production/native test paths.
cJSON *v3_parse(const char *, size_t);
const cJSON *v3_get(const cJSON *, const char *);
int v3_int(const cJSON *, const char *, int fallback);
double v3_num(const cJSON *, const char *, double fallback);
bool v3_bool(const cJSON *, const char *, bool fallback);
const char *v3_str(const cJSON *, const char *, const char *fallback);
bool v3_integer(const cJSON *, double min, double max);
bool v3_keys(const cJSON *, const char *space_separated);
bool v3_decode64(const char *, uint8_t *, size_t capacity, size_t *length);
char *v3_encode64(const uint8_t *, size_t);
size_t v3_frame(const cJSON *, char *, size_t);
void v3_add_u32(cJSON *, const char *, uint32_t);
void v3_json_init(void);
unsigned v3_json_failures(void);
bool v3_put(cJSON *, const char *, cJSON *);
bool v3_push(cJSON *, cJSON *);
bool v3_replace(cJSON *, const char *, cJSON *);
v3_job *v3_schedule(v3_core *, v3_resource *, const char *, const cJSON *);
v3_job *v3_job_find(v3_core *, uint32_t);
bool v3_job_cancel(v3_core *, uint32_t);
void v3_close_tree(v3_core *, v3_resource *);
const char *v3_flow_install(v3_core *, const char *name, const cJSON *, uint32_t *id);
cJSON *v3_flow_operation(v3_core *, const char *, const cJSON *, const char **error);
void v3_flow_tick(v3_core *);
void v3_flow_stop_all(v3_core *);
void v3_flow_destroy(v3_core *);
const char *v3_action_validate(v3_kind, const char *, const cJSON *);
cJSON *v3_actions_json(v3_kind);
const char *v3_operation_validate(const char *, const cJSON *);
bool v3_action_readonly(v3_kind, const char *);
bool v3_exclusive_pending(const v3_core *);
const char *v3_flow_restore(v3_core *, uint32_t, const cJSON *);
void v3_flow_boot_error(v3_core *, uint32_t, const char *);
