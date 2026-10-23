#pragma once
#include "v3.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

typedef struct {
    uint32_t length, id, flags;
    int64_t time;
    uint8_t data[256];
} v3_packet;
typedef struct {
    uint32_t id, instance, pins;
    v3_kind kind;
    int controller, channel, timer, parent_slot;
    uint32_t epoch;
    cJSON *definition;
    void *handle, *secondary, *encoder, *calibration;
    QueueHandle_t events;
    SemaphoreHandle_t done;
    uint8_t *memory;
    size_t memory_size;
    atomic_uint dropped, received, errors, sequence;
    atomic_size_t actual;
    bool installed, enabled, tx_enabled, rx_enabled, adc_configured;
    bool power_held;
    bool shared_memory;
    bool inverted, fade_active;
    bool suspended;
    int64_t next, start, last_edge;
    int level, phase;
} v3_hardware;
extern v3_hardware v3_hardware_slots[V3_RESOURCES];
extern atomic_uint v3_epoch;
typedef struct {
    atomic_uint instance, sequence, dropped, received;
} v3_stream_status;
extern v3_stream_status v3_streams[V3_RESOURCES];
extern atomic_uint v3_lane_slots[8];
bool v3_cancelled(const v3_work *);
int v3_lane(v3_kind, int controller);
int v3_peripheral(v3_work *, cJSON **result, bool *external);
void v3_poll_lane(int lane);
int v3_bus_open(v3_hardware *, const v3_work *);
int v3_bus_close(v3_hardware *);
int v3_bus_call(v3_hardware *, const v3_work *, cJSON **, bool *);
int v3_signal_open(v3_hardware *, const v3_work *);
int v3_signal_close(v3_hardware *);
int v3_signal_call(v3_hardware *, const v3_work *, cJSON **, bool *);
void v3_signal_poll(v3_hardware *, int64_t);
void v3_bus_poll(v3_hardware *);
const cJSON *v3_hw_config(const v3_hardware *);
const cJSON *v3_hw_pins(const v3_hardware *);
v3_hardware *v3_hw_parent(const v3_hardware *);
cJSON *v3_hw_status(v3_hardware *);
int v3_binary_result(const uint8_t *, size_t, cJSON **);
int v3_payload(const cJSON *, uint8_t *, size_t, size_t *);
int v3_timeout(const cJSON *);
void v3_guard_init(void);
bool v3_flash_begin(void);
void v3_flash_end(void);
bool v3_flash_safe(void);
int v3_realtime_begin(v3_hardware *);
void v3_realtime_end(v3_hardware *);
int v3_rmt_sync(const v3_work *, cJSON **, bool *);
