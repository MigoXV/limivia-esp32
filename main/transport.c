#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "limivia.h"
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

// 7b9d0001-7a21-4c9b-8b30-69e5ce1d0001 (service), 0002 RX, 0003 TX.
#define UUID_BYTES(n)                                                                              \
    0x01, 0x00, 0x1d, 0xce, 0xe5, 0x69, 0x30, 0x8b, 0x9b, 0x4c, 0x21, 0x7a, n, 0x00, 0x9d, 0x7b
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(UUID_BYTES(1));
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(UUID_BYTES(2));
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(UUID_BYTES(3));
static uint16_t tx_handle;
static atomic_uint_fast16_t connection = BLE_HS_CONN_HANDLE_NONE;
static atomic_uint_fast32_t ble_generation;
static atomic_bool subscribed;
static uint8_t address_type;
static SemaphoreHandle_t tx_lock;
static char device_name[24];
static atomic_uint_fast32_t usb_last_request;
static struct {
    request_t request;
    size_t used;
    bool dropping;
    int64_t last;
} receivers[2];

static void receive(source_t source, const uint8_t *data, size_t length) {
    int64_t now = esp_timer_get_time();
    if (receivers[source].used && now - receivers[source].last > 3000000) {
        receivers[source].used = 0;
        receivers[source].dropping = true; // discard the incomplete frame through its delimiter
    }
    receivers[source].last = now;
    for (size_t i = 0; i < length; i++) {
        if (data[i] == '\n') {
            request_t *r = &receivers[source].request;
            r->source = source;
            r->generation = source == SOURCE_BLE ? ble_generation : 0;
            if (receivers[source].dropping) {
                strcpy(r->text, "!invalid-frame");
            } else {
                r->text[receivers[source].used] = '\0';
            }
            if (source == SOURCE_USB)
                usb_last_request = (uint32_t)(now / 1000);
            if (receivers[source].used || receivers[source].dropping)
                control_submit(r);
            receivers[source].used = 0;
            receivers[source].dropping = false;
        } else if (!receivers[source].dropping) {
            if (receivers[source].used >= REQUEST_MAX || data[i] == 0) {
                receivers[source].dropping = true;
            } else {
                receivers[source].request.text[receivers[source].used++] = (char)data[i];
            }
        }
    }
}

static void usb_reader(void *arg) {
    uint8_t data[128];
    for (;;) {
        int count = usb_serial_jtag_read_bytes(data, sizeof(data), pdMS_TO_TICKS(50));
        if (count > 0)
            receive(SOURCE_USB, data, count);
    }
}

static int access_gatt(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg) {
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    uint8_t bytes[512];
    uint16_t len;
    if (ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof(bytes), &len))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    receive(SOURCE_BLE, bytes, len);
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
static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (!event->connect.status) {
            connection = event->connect.conn_handle;
            ble_generation++;
        } else
            advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE;
        subscribed = false;
        ble_generation++;
        memset(&receivers[SOURCE_BLE], 0, sizeof(receivers[SOURCE_BLE]));
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_handle)
            subscribed = event->subscribe.cur_notify;
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
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}
static void on_sync(void) {
    if (ble_hs_id_infer_auto(0, &address_type) == 0)
        advertise();
}
static void on_reset(int reason) {
    connection = BLE_HS_CONN_HANDLE_NONE;
    subscribed = false;
    ble_generation++;
    memset(&receivers[SOURCE_BLE], 0, sizeof(receivers[SOURCE_BLE]));
}
static void ble_host(void *arg) {
    nimble_port_run();
    vTaskDelete(NULL);
}

bool transport_ble_connected(void) { return connection != BLE_HS_CONN_HANDLE_NONE; }
bool transport_usb_connected(void) {
    return usb_last_request &&
           (uint32_t)((uint32_t)(esp_timer_get_time() / 1000) - usb_last_request) < 5000;
}

void transport_send(source_t source, uint32_t generation, const cJSON *message) {
    char *json = cJSON_PrintUnformatted(message);
    if (!json)
        return;
    size_t length = strlen(json);
    char *line = malloc(length + 2);
    if (!line) {
        free(json);
        return;
    }
    memcpy(line, json, length);
    line[length++] = '\n';
    free(json);
    if (xSemaphoreTake(tx_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        free(line);
        return;
    }
    if (source == SOURCE_USB) {
        if (transport_usb_connected())
            usb_serial_jtag_write_bytes(line, length, pdMS_TO_TICKS(100));
    } else if (subscribed && connection != BLE_HS_CONN_HANDLE_NONE &&
               generation == ble_generation) {
        const uint16_t conn = connection;
        uint16_t mtu = ble_att_mtu(conn);
        size_t chunk = mtu > 3 ? mtu - 3 : 20;
        if (chunk > 180)
            chunk = 180;
        for (size_t offset = 0; offset < length && generation == ble_generation;) {
            size_t count = length - offset < chunk ? length - offset : chunk;
            bool sent = false;
            for (int attempt = 0; attempt < 10 && generation == ble_generation; attempt++) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(line + offset, count);
                if (om && ble_gatts_notify_custom(conn, tx_handle, om) == 0) {
                    sent = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (!sent)
                break;
            offset += count;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    xSemaphoreGive(tx_lock);
    free(line);
}
void transport_broadcast(const cJSON *message) {
    transport_send(SOURCE_USB, 0, message);
    transport_send(SOURCE_BLE, ble_generation, message);
}
void transport_init(void) {
    tx_lock = xSemaphoreCreateMutex();
    configASSERT(tx_lock);
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 2048, .tx_buffer_size = 8192};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(device_name, sizeof(device_name), "LIMIVIA-%02X%02X", mac[4], mac[5]);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(device_name);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    configASSERT(xTaskCreate(ble_host, "ble", 4096, NULL, 5, NULL) == pdPASS);
    configASSERT(xTaskCreate(usb_reader, "usb-rx", 4096, NULL, 5, NULL) == pdPASS);
}
