#include "limivia.h"
#include "tx.h"
#include "ble_wire.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define UUID_BYTES(n)                                                                              \
    0x01, 0x00, 0x1d, 0xce, 0xe5, 0x69, 0x30, 0x8b, 0x9b, 0x4c, 0x21, 0x7a, n, 0x00, 0x9d, 0x7b
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(UUID_BYTES(1));
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(UUID_BYTES(2));
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(UUID_BYTES(3));
static uint16_t tx_handle;
static atomic_uint connection = BLE_HS_CONN_HANDLE_NONE, generation, mtu = 23;
static atomic_bool subscribed;
static atomic_uint interval_us;
static atomic_bool parameters_requested, parameters_updated;
static atomic_uint parameter_request_status;
static atomic_uint connects, connect_failures, disconnects, last_disconnect, parameter_status;
static void connection_parameters(void);
void transport_ble_diagnostics(cJSON *p) {
    connection_parameters();
    cJSON_AddBoolToObject(p, "ble_parameter_requested", atomic_load(&parameters_requested));
    cJSON_AddBoolToObject(p, "ble_parameter_updated", atomic_load(&parameters_updated));
    cJSON_AddNumberToObject(p, "ble_parameter_request_status", atomic_load(&parameter_request_status));
    cJSON_AddBoolToObject(p, "ble_connected", transport_ble_connected());
    cJSON_AddNumberToObject(p, "ble_connects", atomic_load(&connects));
    cJSON_AddNumberToObject(p, "ble_connect_failures", atomic_load(&connect_failures));
    cJSON_AddNumberToObject(p, "ble_disconnects", atomic_load(&disconnects));
    cJSON_AddNumberToObject(p, "ble_last_disconnect_reason", atomic_load(&last_disconnect));
    cJSON_AddNumberToObject(p, "ble_parameter_status", atomic_load(&parameter_status));
}
static uint8_t address_type;
static char device_name[24];
static lv_rx receivers[2]; // WebSocket/MQTT use their own connection-bound frame assembly.

// Notifications and disconnects execute on the NimBLE host event queue, where
// GAP handle reuse is serialized with the final generation check.
static struct {
    struct ble_npl_event event;
    char bytes[512];
    size_t size;
    uint32_t generation;
    int64_t deadline;
    atomic_bool busy;
    int result;
    SemaphoreHandle_t done;
} notification;
static struct {
    struct ble_npl_event event;
    uint32_t generation;
    uint16_t handle;
} termination;

uint32_t transport_generation(lv_source source) {
    return source == LV_BLE   ? atomic_load(&generation)
           : source == LV_USB ? 0
                              : network_generation(source);
}
bool transport_ble_connected(void) { return atomic_load(&connection) != BLE_HS_CONN_HANDLE_NONE; }
uint16_t transport_ble_mtu(void) { return atomic_load(&mtu); }
uint32_t transport_ble_interval_us(void) { return atomic_load(&interval_us); }
static void connection_parameters(void) {
    struct ble_gap_conn_desc desc;
    if (!ble_gap_conn_find(atomic_load(&connection), &desc))
        atomic_store(&interval_us, (unsigned)desc.conn_itvl * 1250);
}
static bool current(void *ctx, uint32_t epoch) {
    (void)ctx;
    return epoch == atomic_load(&generation) && subscribed && transport_ble_connected();
}
static void notify_on_host(struct ble_npl_event *event) {
    (void)event;
    notification.result = -1;
    if (current(NULL, notification.generation) &&
        esp_timer_get_time() / 1000 < notification.deadline) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(notification.bytes, notification.size);
        // notify_custom consumes om on success AND failure.
        notification.result =
            om && !ble_gatts_notify_custom(atomic_load(&connection), tx_handle, om) ? 1 : 0;
    }
    atomic_store(&notification.busy, false);
    xSemaphoreGive(notification.done);
}
static void terminate_on_host(struct ble_npl_event *event) {
    (void)event;
    if (termination.generation == atomic_load(&generation) &&
        termination.handle == atomic_load(&connection))
        ble_gap_terminate(termination.handle, BLE_ERR_REM_USER_CONN_TERM);
}
static void abort_frame(void *ctx, uint32_t epoch) {
    (void)ctx;
    if (!current(NULL, epoch))
        return;
    subscribed = false;
    termination.handle = atomic_load(&connection);
    termination.generation = atomic_fetch_add(&generation, 1) + 1;
    control_wake();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &termination.event);
}
static int fragment(void *ctx, uint32_t epoch, const char *bytes, size_t size, int64_t deadline) {
    (void)ctx;
    if (atomic_load(&notification.busy))
        return 0;
    xSemaphoreTake(notification.done, 0);
    memcpy(notification.bytes, bytes, size);
    notification.size = size;
    notification.generation = epoch;
    notification.deadline = deadline;
    atomic_store(&notification.busy, true);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &notification.event);
    while (esp_timer_get_time() / 1000 < deadline) {
        if (xSemaphoreTake(notification.done, pdMS_TO_TICKS(10)) == pdTRUE)
            return notification.result;
        if (!current(NULL, epoch))
            return -1;
    }
    return -1; // static job remains owned by the host until it finishes
}
static int64_t now(void *ctx) {
    (void)ctx;
    return esp_timer_get_time() / 1000;
}
static void pause_tx(void *ctx) {
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(2));
}
bool transport_send(lv_source source, uint32_t epoch, const char *text, size_t size) {
    if (source == LV_USB) {
        // ESP-IDF 6.0.2 returns the entire count or zero for this ring-buffer API.
        return usb_serial_jtag_write_bytes(text, size, pdMS_TO_TICKS(100)) == (int)size;
    }
    if (source != LV_BLE)
        return network_send(source, epoch, text, size);
    size_t payload = atomic_load(&mtu) > 3 ? atomic_load(&mtu) - 3 : 20;
    if (payload > sizeof(notification.bytes))
        payload = sizeof(notification.bytes);
    const char *bytes = text;
    uint8_t *packed = NULL;
#ifdef LV_BLE_BINARY
    if (size && text[size - 1] == '\n') --size;
    packed = malloc(size + LV_WIRE_HEADER);
    size_t packed_size = packed ? lv_wire_pack((const uint8_t *)text, size, packed, size + LV_WIRE_HEADER) : 0;
    if (!packed_size) { free(packed); abort_frame(NULL, epoch); return false; }
    bytes = (const char *)packed;
    size = packed_size;
#endif
    bool ok = lv_tx_frame((lv_tx_io){.now = now,
                                  .current = current,
                                  .fragment = fragment,
                                  .pause = pause_tx,
                                  .abort = abort_frame},
                       epoch, payload, bytes, size);
    free(packed);
    return ok;
}
static void frame(void *ctx, const char *text, size_t n) {
    lv_source source = (lv_source)(intptr_t)ctx;
    control_submit(source, transport_generation(source), text, n);
}
#ifdef LV_BLE_BINARY
static struct { uint8_t bytes[LV_REQUEST_MAX + LV_WIRE_HEADER]; size_t used, expected; int64_t last; } binary_rx;
static bool receive_binary(const uint8_t *bytes, size_t n) {
    int64_t at = esp_timer_get_time() / 1000;
    if (binary_rx.used && at - binary_rx.last > 3000) return false;
    binary_rx.last = at;
    while (n) {
        size_t expected = binary_rx.expected ? binary_rx.expected : LV_WIRE_HEADER;
        size_t take = expected - binary_rx.used;
        if (take > n) take = n;
        memcpy(binary_rx.bytes + binary_rx.used, bytes, take);
        binary_rx.used += take; bytes += take; n -= take;
        if (binary_rx.used < LV_WIRE_HEADER) continue;
        if (!binary_rx.expected) {
            binary_rx.expected = lv_wire_size(binary_rx.bytes, LV_REQUEST_MAX);
            if (!binary_rx.expected) return false;
        }
        if (binary_rx.used == binary_rx.expected) {
            uint8_t text[LV_REQUEST_MAX + 1]; size_t used;
            if (!lv_wire_unpack(binary_rx.bytes, binary_rx.used, text, LV_REQUEST_MAX, &used)) return false;
            text[used] = 0;
            frame((void *)(intptr_t)LV_BLE, (const char *)text, used);
            binary_rx.used = binary_rx.expected = 0;
        }
    }
    return true;
}
#endif
static void receive(lv_source source, const uint8_t *bytes, size_t n) {
#ifdef LV_BLE_BINARY
    if (source == LV_BLE) {
        if (!receive_binary(bytes, n)) abort_frame(NULL, atomic_load(&generation));
        return;
    }
#endif
    lv_receive(&receivers[source], bytes, n, esp_timer_get_time() / 1000, frame,
               (void *)(intptr_t)source);
}
static void usb_reader(void *arg) {
    (void)arg;
    uint8_t bytes[128];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(50));
        if (n > 0)
            receive(LV_USB, bytes, n);
    }
}
static int access_gatt(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    if (!subscribed)
        return BLE_ATT_ERR_UNLIKELY;
    uint8_t bytes[512];
    uint16_t n;
    if (ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof(bytes), &n))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    receive(LV_BLE, bytes, n);
    return 0;
}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &service_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &rx_uuid.u, .access_cb = access_gatt, .flags = BLE_GATT_CHR_F_WRITE},
             {.uuid = &tx_uuid.u,
              .access_cb = access_gatt,
              .val_handle = &tx_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY},
             {0}}},
    {0}};
static void advertise(void);
static void disconnected(void) {
    connection = BLE_HS_CONN_HANDLE_NONE;
    subscribed = false;
    mtu = 23;
    interval_us = 0;
    parameters_requested = false;
    parameters_updated = false;
    parameter_request_status = 0;
    atomic_fetch_add(&generation, 1);
    memset(&receivers[LV_BLE], 0, sizeof(receivers[LV_BLE]));
#ifdef LV_BLE_BINARY
    memset(&binary_rx, 0, sizeof(binary_rx));
#endif
    control_wake();
}
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (!event->connect.status) {
            connection = event->connect.conn_handle;
            atomic_fetch_add(&connects, 1);
            atomic_fetch_add(&generation, 1);
            control_wake();
        } else {
            atomic_fetch_add(&connect_failures, 1);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        atomic_fetch_add(&disconnects, 1);
        atomic_store(&last_disconnect, event->disconnect.reason);
        disconnected();
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_handle) {
            subscribed = event->subscribe.cur_notify;
            // Wait for a usable GATT link. One proposal per connection; rejection
            // leaves the central's parameters in force and never blocks an RPC.
            if (subscribed && !atomic_exchange(&parameters_requested, true)) {
                const struct ble_gap_upd_params params = {
                    .itvl_min = 12, .itvl_max = 24, .latency = 0,
                    .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0};
                atomic_store(&parameter_request_status,
                             ble_gap_update_params(event->subscribe.conn_handle, &params));
            }

            if (!subscribed) {
                memset(&receivers[LV_BLE], 0, sizeof(receivers[LV_BLE]));
#ifdef LV_BLE_BINARY
    memset(&binary_rx, 0, sizeof(binary_rx));
#endif
                atomic_fetch_add(&generation, 1);
                control_wake();
            }
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        atomic_store(&parameter_status, event->conn_update.status);
        atomic_store(&parameters_updated, event->conn_update.status == 0);
        break;
    case BLE_GAP_EVENT_MTU:
        mtu = event->mtu.value;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}
static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields))
        return;
    struct ble_hs_adv_fields response = {0};
    response.name = (uint8_t *)device_name;
    response.name_len = strlen(device_name);
    response.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&response))
        return;
    struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND,
                                        .disc_mode = BLE_GAP_DISC_MODE_GEN};
    ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}
static void on_sync(void) {
    if (!ble_hs_id_infer_auto(0, &address_type))
        advertise();
}
static void on_reset(int reason) {
    (void)reason;
    disconnected();
}
static void ble_host(void *arg) {
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}
void transport_init(void) {
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 2048, .tx_buffer_size = 8200};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    snprintf(device_name, sizeof(device_name), "LIMIVIA-%02X%02X", mac[4], mac[5]);
    ESP_ERROR_CHECK(nimble_port_init());
    notification.done = xSemaphoreCreateBinary();
    configASSERT(notification.done);
    ble_npl_event_init(&notification.event, notify_on_host, NULL);
    ble_npl_event_init(&termination.event, terminate_on_host, NULL);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(device_name);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    configASSERT(xTaskCreate(ble_host, "ble-host", 6144, NULL, 5, NULL) == pdPASS);
    configASSERT(xTaskCreate(usb_reader, "usb-rx", 6144, NULL, 5, NULL) == pdPASS);
}
