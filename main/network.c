#include "limivia.h"
#include "services.h"
#include "peripherals.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static atomic_uint routes[6], wifi_events, mqtt_redeliveries, mqtt_retained, mqtt_dropped,
    now_dropped;
static atomic_bool wifi_connected, mqtt_connected, maintenance, wifi_wants_connection, now_pending;
static bool wifi_started, now_started, wifi_initialized;
static atomic_bool network_ready;
static int wifi_error;
static int ensure_driver(void);
static int init_error;
static httpd_handle_t server;
static SemaphoreHandle_t route_lock, now_done, mqtt_guard;
static int ws_fd[2] = {-1, -1};
static _Atomic(esp_mqtt_client_handle_t) mqtt;
static cJSON *mqtt_settings;
static char *mqtt_ca, *mqtt_certificate, *mqtt_private_key;
static int mqtt_error;
static void restore_network(void);
static char mqtt_prefix[96];
typedef struct {
    char name[33];
    int64_t last;
} logical_client;
static logical_client logical[4];
static QueueHandle_t now_received, telemetry;
typedef struct {
    int64_t time;
    size_t length;
    char topic[160];
    uint8_t data[LV_REQUEST_MAX];
} net_packet;
static atomic_int now_result;
static struct {
    size_t used, total;
    char text[LV_REQUEST_MAX + 1], topic[160];
    bool retained, duplicate;
} mqtt_rx;
static atomic_int_fast64_t reconnect_at;
static esp_netif_t *sta, *ap;
uint32_t network_generation(lv_source source) {
    return source >= LV_WS0 && source <= LV_MQTT3 ? atomic_load(&routes[source - LV_WS0]) : 0;
}
static void ws_closed(void *ctx) {
    int slot = (intptr_t)ctx - 1;
    if (slot < 0 || slot >= 2)
        return;
    xSemaphoreTake(route_lock, portMAX_DELAY);
    ws_fd[slot] = -1;
    atomic_fetch_add(&routes[slot], 1);
    xSemaphoreGive(route_lock);
    control_wake();
}
// IDF 6 does not invoke the frame handler for the HTTP upgrade itself.
static esp_err_t ws_connected(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req), one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
        return ESP_FAIL;
    xSemaphoreTake(route_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < 2; ++i)
        if (ws_fd[i] < 0) {
            slot = i;
            break;
        }
    if (slot >= 0) {
        ws_fd[slot] = fd;
        atomic_fetch_add(&routes[slot], 1);
        req->sess_ctx = (void *)(intptr_t)(slot + 1);
        req->free_ctx = ws_closed;
    }
    xSemaphoreGive(route_lock);
    return slot < 0 ? ESP_ERR_NO_MEM : ESP_OK;
}
static esp_err_t ws_handler(httpd_req_t *req) {
    int slot = (intptr_t)req->sess_ctx - 1;
    if (slot < 0 || slot >= 2)
        return ESP_FAIL;
    httpd_ws_frame_t frame = {0};
    int err = httpd_ws_recv_frame(req, &frame, 0);
    if (err)
        return err;
    if (frame.type != HTTPD_WS_TYPE_TEXT || !frame.len || frame.len > LV_REQUEST_MAX + 1 || !frame.final)
        return ESP_ERR_INVALID_ARG;
    char *text = malloc(frame.len + 1);
    if (!text)
        return ESP_ERR_NO_MEM;
    frame.payload = (uint8_t *)text;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (!err) {
        size_t n = frame.len;
        if (n && text[n - 1] == '\n')
            --n;
        text[n] = 0;
        control_submit(LV_WS0 + slot, network_generation(LV_WS0 + slot), text, n);
    }
    free(text);
    return err;
}
bool network_send(lv_source source, uint32_t generation, const char *text, size_t n) {
    if (!route_lock)
        return false;
    if (LV_HAS_IOT && source >= LV_MQTT0 && source <= LV_MQTT3) {
        if (!mqtt_guard || xSemaphoreTake(mqtt_guard, pdMS_TO_TICKS(1000)) != pdTRUE)
            return false;
        xSemaphoreTake(route_lock, portMAX_DELAY);
        esp_mqtt_client_handle_t client = NULL;
        char topic[160];
        if (generation == network_generation(source) && mqtt && atomic_load(&mqtt_connected)) {
            client = mqtt;
            snprintf(topic, sizeof(topic), "%s/reply/%s", mqtt_prefix,
                     logical[source - LV_MQTT0].name);
        }
        xSemaphoreGive(route_lock);
        // mqtt's event callbacks execute under its API lock and take route_lock.
        // Never hold route_lock across a vendor call. mqtt_guard pins client lifetime;
        // callbacks do not take mqtt_guard, so callback -> route and TX -> mqtt cannot cycle.
        bool ok = client && esp_mqtt_client_enqueue(client, topic, text, (int)n, 1, 0, true) >= 0;
        xSemaphoreGive(mqtt_guard);
        return ok;
    }
    xSemaphoreTake(route_lock, portMAX_DELAY);
    bool ok = false;
    if (generation != network_generation(source))
        goto done;
    if (source == LV_WS0 || source == LV_WS1) {
        int slot = source - LV_WS0;
        if (server && ws_fd[slot] >= 0 &&
            httpd_ws_get_fd_info(server, ws_fd[slot]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = n};
            ok = httpd_ws_send_frame_async(server, ws_fd[slot], &f) == ESP_OK;
        }
    }
done:
    xSemaphoreGive(route_lock);
    return ok;
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *event) {
    (void)arg;
    (void)event;
    atomic_fetch_add(&wifi_events, 1);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        atomic_store(&wifi_connected, true);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&wifi_connected, false);
        if (atomic_load(&wifi_wants_connection))
            reconnect_at = esp_timer_get_time() / 1000 + 2000;
    }
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        atomic_store(&mqtt_connected, true);
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/command/+", mqtt_prefix);
        esp_mqtt_client_subscribe(e->client, topic, 1);
        return;
    }
    if (id == MQTT_EVENT_DISCONNECTED) {
        atomic_store(&mqtt_connected, false);
        return;
    }
    if (id != MQTT_EVENT_DATA)
        return;
    if (e->current_data_offset == 0) {
        memset(&mqtt_rx, 0, sizeof(mqtt_rx));
        mqtt_rx.total = e->total_data_len;
        mqtt_rx.retained = e->retain;
        mqtt_rx.duplicate = e->dup;
        if (e->topic_len <= 0 || e->topic_len >= (int)sizeof(mqtt_rx.topic)) {
            atomic_fetch_add(&mqtt_dropped, 1);
            return;
        }
        memcpy(mqtt_rx.topic, e->topic, e->topic_len);
    }
    if (mqtt_rx.total > LV_REQUEST_MAX || e->current_data_offset != (int)mqtt_rx.used ||
        mqtt_rx.used + e->data_len > LV_REQUEST_MAX) {
        atomic_fetch_add(&mqtt_dropped, 1);
        return;
    }
    memcpy(mqtt_rx.text + mqtt_rx.used, e->data, e->data_len);
    mqtt_rx.used += e->data_len;
    if (mqtt_rx.used != mqtt_rx.total)
        return;
    mqtt_rx.text[mqtt_rx.used] = 0;
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s/command/", mqtt_prefix);
    size_t prefix_len = strlen(prefix);
    if (strncmp(mqtt_rx.topic, prefix, prefix_len)) {
        net_packet p = {.time = esp_timer_get_time(), .length = mqtt_rx.used};
        strcpy(p.topic, mqtt_rx.topic);
        memcpy(p.data, mqtt_rx.text, p.length);
        if (xQueueSend(telemetry, &p, 0) != pdTRUE)
            atomic_fetch_add(&mqtt_dropped, 1);
        return;
    }
    if (mqtt_rx.retained) {
        atomic_fetch_add(&mqtt_retained, 1);
    }
    const char *name = mqtt_rx.topic + prefix_len;
    size_t length = strlen(name);
    if (!length || length > 32 || strchr(name, '/'))
        return;
    int64_t now = esp_timer_get_time() / 1000;
    if (mqtt_rx.duplicate)
        atomic_fetch_add(&mqtt_redeliveries, 1);
    // Session sequences prevent duplicate writes; hello's nonce preserves its session.
    xSemaphoreTake(route_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < 4; ++i)
        if (!strcmp(logical[i].name, name)) {
            slot = i;
            break;
        }
    if (slot < 0)
        for (int i = 0; i < 4; ++i)
            if (!logical[i].name[0] || now - logical[i].last > 5000) {
                slot = i;
                snprintf(logical[i].name, sizeof(logical[i].name), "%s", name);
                atomic_fetch_add(&routes[2 + i], 1);
                break;
            }
    if (slot >= 0)
        logical[slot].last = now;
    xSemaphoreGive(route_lock);
    if (slot >= 0)
        control_submit_message(LV_MQTT0 + slot, network_generation(LV_MQTT0 + slot), mqtt_rx.text,
                               mqtt_rx.used, mqtt_rx.retained);
    else
        atomic_fetch_add(&mqtt_dropped, 1);
}
static void now_rx(const esp_now_recv_info_t *info, const uint8_t *data, int size) {
    v3_packet p = {.time = esp_timer_get_time()};
    p.length = size > 250 ? 256 : size + 6;
    memcpy(p.data, info->src_addr, 6);
    memcpy(p.data + 6, data, p.length - 6);
    p.flags = size > 250 ? 1 : 0;
    if (xQueueSend(now_received, &p, 0) != pdTRUE)
        atomic_fetch_add(&now_dropped, 1);
}
static void now_tx(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    (void)info;
    atomic_store(&now_result, status == ESP_NOW_SEND_SUCCESS ? 1 : 2);
    xSemaphoreGive(now_done);
    atomic_store(&now_pending, false);
}
void v3_network_init(void) {
    route_lock = xSemaphoreCreateMutex();
    now_done = xSemaphoreCreateBinary();
    if (LV_HAS_IOT) {
        mqtt_guard = xSemaphoreCreateMutex();
        now_received = xQueueCreate(8, sizeof(v3_packet));
        telemetry = xQueueCreate(4, sizeof(net_packet));
    }
    if (!route_lock || !now_done || (LV_HAS_IOT && (!mqtt_guard || !now_received || !telemetry))) {
        init_error = ESP_ERR_NO_MEM;
        return;
    }
    if ((init_error = esp_netif_init()))
        return;
    int err = esp_event_loop_create_default();
    if (err && err != ESP_ERR_INVALID_STATE) {
        init_error = err;
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mqtt_prefix, sizeof(mqtt_prefix), "limivia/%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    restore_network();
    atomic_store(&network_ready, true);
}
bool v3_network_maintenance(void) { return atomic_load(&maintenance); }
static bool parse_mac(const char *text, uint8_t mac[6]) {
    if (strlen(text) != 17)
        return false;
    unsigned m[6];
    char tail;
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5],
               &tail) != 6)
        return false;
    for (int i = 0; i < 6; ++i)
        mac[i] = m[i];
    return true;
}
static int ensure_driver(void) {
    if (init_error)
        return init_error;
    if (!wifi_initialized) {
        if (!sta)
            sta = esp_netif_create_default_wifi_sta();
        if (!ap)
            ap = esp_netif_create_default_wifi_ap();
        if (!sta || !ap)
            return ESP_ERR_NO_MEM;
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        wifi_error = esp_wifi_init(&cfg);
        if (wifi_error)
            return wifi_error;
        wifi_initialized = true;
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (!server) {
        httpd_config_t http = HTTPD_DEFAULT_CONFIG();
        http.max_open_sockets = 3;
        http.stack_size = 4096;
        http.send_wait_timeout = 1;
        http.recv_wait_timeout = 1;
        wifi_error = httpd_start(&server, &http);
        if (wifi_error)
            return wifi_error;
        httpd_uri_t uri = {
            .uri = "/v3", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
            .ws_post_handshake_cb = ws_connected};
        wifi_error = httpd_register_uri_handler(server, &uri);
        if (wifi_error)
            return wifi_error;
    }
    return ESP_OK;
}
static int ensure_wifi(void) {
    int driver = ensure_driver();
    if (driver)
        return driver;
    if (init_error)
        return init_error;
    if (wifi_started)
        return ESP_OK;
    int err = esp_wifi_start();
    if (!err)
        wifi_started = true;
    return err;
}
int v3_network_modem_sleep(const char *mode) {
    wifi_ps_type_t ps;
    if (!strcmp(mode, "none"))
        ps = WIFI_PS_NONE;
    else if (!strcmp(mode, "min"))
        ps = WIFI_PS_MIN_MODEM;
    else if (!strcmp(mode, "max"))
        ps = WIFI_PS_MAX_MODEM;
    else
        return ESP_ERR_INVALID_ARG;
    int err = ensure_wifi();
    return err ? err : esp_wifi_set_ps(ps);
}
static int wifi_work(const char *op, const cJSON *p, cJSON **out) {
    if (!strcmp(op, "wifi.status")) {
        *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(*out, "init_error", init_error);
        cJSON_AddBoolToObject(*out, "started", wifi_started);
        cJSON_AddBoolToObject(*out, "initialized", wifi_initialized);
        cJSON_AddNumberToObject(*out, "driver_error", wifi_error);
        cJSON_AddBoolToObject(*out, "connected", atomic_load(&wifi_connected));
        cJSON_AddBoolToObject(*out, "maintenance_ap", atomic_load(&maintenance));
        cJSON_AddNumberToObject(*out, "events", atomic_load(&wifi_events));
        esp_netif_ip_info_t ip;
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
            char addr[16];
            snprintf(addr, sizeof(addr), IPSTR, IP2STR(&ip.ip));
            cJSON_AddStringToObject(*out, "ip", addr);
        }
        return ESP_OK;
    }
    int driver = ensure_driver();
    if (driver)
        return driver;
    if (!strcmp(op, "wifi.configure")) {
        const char *ssid = v3_str(p, "ssid", ""), *password = v3_str(p, "password", "");
        if (!*ssid || strlen(ssid) > 32 || strlen(password) > 63)
            return ESP_ERR_INVALID_ARG;
        if (atomic_load(&maintenance))
            return ESP_ERR_INVALID_STATE;
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, ssid, strlen(ssid));
        memcpy(config.sta.password, password, strlen(password));
        atomic_store(&wifi_wants_connection, true);
        esp_wifi_disconnect();
        int err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err)
            return err;
        if (v3_bool(p, "persist", false)) {
            if (v3_nvs_error)
                return v3_nvs_error;
            nvs_handle_t n;
            if ((err = nvs_open("limivia_wifi", NVS_READWRITE, &n)))
                return err;
            err = nvs_set_blob(n, "sta", &config.sta, sizeof(config.sta));
            if (!err)
                err = nvs_commit(n);
            nvs_close(n);
            if (err)
                return err;
        }
        if ((err = ensure_wifi()))
            return err;
        return esp_wifi_connect();
    }
    if (!strcmp(op, "wifi.scan")) {
        int err = ensure_wifi();
        if (err)
            return err;
        wifi_scan_config_t scan = {0};
        if ((err = esp_wifi_scan_start(&scan, true)))
            return err;
        uint16_t count = 16;
        wifi_ap_record_t *records = calloc(count, sizeof(*records));
        if (!records)
            return ESP_ERR_NO_MEM;
        err = esp_wifi_scan_get_ap_records(&count, records);
        if (!err) {
            *out = cJSON_CreateObject();
            cJSON *a = cJSON_AddArrayToObject(*out, "networks");
            for (int i = 0; i < count; ++i) {
                cJSON *r = cJSON_CreateObject();
                char *encoded =
                    v3_encode64(records[i].ssid, strnlen((const char *)records[i].ssid, 32));
                cJSON_AddStringToObject(r, "ssid_base64", encoded ? encoded : "");
                free(encoded);
                cJSON_AddNumberToObject(r, "rssi", records[i].rssi);
                cJSON_AddNumberToObject(r, "channel", records[i].primary);
                cJSON_AddNumberToObject(r, "auth", records[i].authmode);
                v3_push(a, r);
            }
        }
        free(records);
        return err;
    }
    if (!strcmp(op, "wifi.maintenance")) {
        bool enable = v3_bool(p, "enable", false);
        int err;
        if (enable) {
            if (LV_HAS_IOT) {
                xSemaphoreTake(mqtt_guard, portMAX_DELAY);
                xSemaphoreTake(route_lock, portMAX_DELAY);
                for (int i = 2; i < 6; ++i)
                    atomic_fetch_add(&routes[i], 1);
                xSemaphoreGive(route_lock);
                if (mqtt)
                    esp_mqtt_client_stop(mqtt);
                atomic_store(&mqtt_connected, false);
                xSemaphoreGive(mqtt_guard);
                control_wake();
            }
            if (LV_HAS_IOT && now_started) {
                esp_now_deinit();
                now_started = false;
            }
            esp_wifi_disconnect();
            atomic_store(&wifi_wants_connection, false);
            atomic_store(&maintenance, true);
            wifi_config_t config = {.ap = {.ssid = "LIMIVIA-Maintenance",
                                           .ssid_len = 19,
                                           .channel = 1,
                                           .max_connection = 2,
                                           .authmode = WIFI_AUTH_OPEN}};
            if ((err = esp_wifi_set_mode(WIFI_MODE_AP)))
                return err;
            if ((err = esp_wifi_set_config(WIFI_IF_AP, &config)))
                return err;
            return ensure_wifi();
        }
        if ((err = esp_wifi_set_mode(WIFI_MODE_STA)))
            return err;
        atomic_store(&maintenance, false);
        return ESP_OK;
    }
    if (!strcmp(op, "wifi.disconnect")) {
        atomic_store(&wifi_wants_connection, false);
        reconnect_at = 0;
        return esp_wifi_disconnect();
    }
    return ESP_ERR_NOT_SUPPORTED;
}
static int mqtt_work_impl(const char *op, const cJSON *p, cJSON **out) {
    if (!strcmp(op, "mqtt.status")) {
        *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(*out, "configured", mqtt != NULL);
        cJSON_AddBoolToObject(*out, "connected", atomic_load(&mqtt_connected));
        cJSON_AddNumberToObject(*out, "last_error", mqtt_error);
        cJSON_AddBoolToObject(*out, "password_set", *v3_str(mqtt_settings, "password", "") != 0);
        cJSON_AddNumberToObject(*out, "redeliveries", atomic_load(&mqtt_redeliveries));
        cJSON_AddNumberToObject(*out, "retained_commands_rejected", atomic_load(&mqtt_retained));
        cJSON_AddNumberToObject(*out, "dropped", atomic_load(&mqtt_dropped));
        return ESP_OK;
    }
    if (atomic_load(&maintenance))
        return ESP_ERR_INVALID_STATE;
    if (!strcmp(op, "mqtt.configure")) {
        const char *uri = v3_str(p, "uri", ""), *protocol = v3_str(p, "protocol", "3.1.1");
        if ((strncmp(uri, "mqtt://", 7) && strncmp(uri, "mqtts://", 8)) || strlen(uri) > 256 ||
            strchr(uri, '@') || (strcmp(protocol, "3.1.1") && strcmp(protocol, "5.0")))
            return ESP_ERR_INVALID_ARG;
        xSemaphoreTake(route_lock, portMAX_DELAY);
        esp_mqtt_client_handle_t old = mqtt;
        mqtt = NULL;
        for (int i = 2; i < 6; ++i)
            atomic_fetch_add(&routes[i], 1);
        xSemaphoreGive(route_lock);
        if (old) {
            esp_mqtt_client_stop(old);
            esp_mqtt_client_destroy(old);
        }
        free(mqtt_ca);
        mqtt_ca = NULL;
        free(mqtt_certificate);
        mqtt_certificate = NULL;
        free(mqtt_private_key);
        mqtt_private_key = NULL;
        cJSON_Delete(mqtt_settings);
        mqtt_settings = cJSON_Duplicate(p, true);
        if (!mqtt_settings)
            return ESP_ERR_NO_MEM;
        p = mqtt_settings;
        uri = v3_str(p, "uri", "");
        protocol = v3_str(p, "protocol", "3.1.1");
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = uri,
            .session.protocol_ver =
                !strcmp(protocol, "5.0") ? MQTT_PROTOCOL_V_5 : MQTT_PROTOCOL_V_3_1_1,
            .credentials.username = v3_str(p, "username", NULL),
            .credentials.authentication.password = v3_str(p, "password", NULL),
            .session.last_will = {.topic = v3_str(p, "will_topic", NULL),
                                  .msg = v3_str(p, "will", NULL),
                                  .qos = v3_int(p, "will_qos", 1),
                                  .retain = v3_bool(p, "will_retain", true)},
            .buffer.size = 2048,
            // esp-mqtt fragments large enqueue payloads into its owned outbox.
            .buffer.out_size = 2048,
            .outbox.limit = 16384};
        const char *ca = v3_str(p, "ca_pem", NULL);
        size_t file_size = 0;
        int file_error;
        if (v3_get(p, "ca_file")) {
            if ((file_error =
                     v3_storage_read_bytes(v3_str(p, "ca_file", ""), false, &mqtt_ca, &file_size)))
                return file_error;
            ca = mqtt_ca;
        }
        if (v3_get(p, "certificate_file")) {
            if ((file_error = v3_storage_read_bytes(v3_str(p, "certificate_file", ""), false,
                                                    &mqtt_certificate, &file_size)))
                return file_error;
        }
        if (v3_get(p, "private_key_file")) {
            if ((file_error = v3_storage_read_bytes(v3_str(p, "private_key_file", ""), true,
                                                    &mqtt_private_key, &file_size)))
                return file_error;
        }
        if ((mqtt_certificate == NULL) != (mqtt_private_key == NULL))
            return ESP_ERR_INVALID_ARG;
        cfg.credentials.authentication.certificate = mqtt_certificate;
        cfg.credentials.authentication.key = mqtt_private_key;
        cfg.broker.verification.certificate = ca;
        if (!strncmp(uri, "mqtts://", 8) && !ca)
            return ESP_ERR_INVALID_ARG;
        mqtt = esp_mqtt_client_init(&cfg);
        if (!mqtt)
            return ESP_ERR_NO_MEM;
        int err = esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
        if (err)
            return err;
        if (v3_bool(p, "persist", false)) {
            if (v3_nvs_error)
                return v3_nvs_error;
            nvs_handle_t n;
            err = nvs_open("limivia_mqtt", NVS_READWRITE, &n);
            if (err)
                return err;
            char *json = cJSON_PrintUnformatted(p);
            if (!json) {
                nvs_close(n);
                return ESP_ERR_NO_MEM;
            }
            err = nvs_set_str(n, "settings", json);
            cJSON_free(json);
            if (!err)
                err = nvs_commit(n);
            nvs_close(n);
            if (err)
                return err;
        }
        mqtt_error = esp_mqtt_client_start(mqtt);
        return mqtt_error;
    }
    if (!mqtt)
        return ESP_ERR_INVALID_STATE;
    if (!strcmp(op, "mqtt.publish")) {
        int qos = v3_int(p, "qos", 0);
        const char *topic = v3_str(p, "topic", "");
        if (qos < 0 || qos > 2 || !*topic || strlen(topic) > 128)
            return ESP_ERR_INVALID_ARG;
        uint8_t *bytes = malloc(V3_BUFFER_MAX);
        if (!bytes)
            return ESP_ERR_NO_MEM;
        size_t n = 0;
        int err = ESP_OK;
        if (v3_get(p, "json")) {
            if (v3_get(p, "data")) {
                free(bytes);
                return ESP_ERR_INVALID_ARG;
            }
            char *text = cJSON_PrintUnformatted(v3_get(p, "json"));
            if (!text) {
                free(bytes);
                return ESP_ERR_NO_MEM;
            }
            n = strlen(text);
            if (n > V3_BUFFER_MAX)
                err = ESP_ERR_INVALID_SIZE;
            else
                memcpy(bytes, text, n);
            cJSON_free(text);
        } else
            err = v3_payload(p, bytes, V3_BUFFER_MAX, &n);
        if (!err) {
            int id = esp_mqtt_client_enqueue(mqtt, topic, n ? (const char *)bytes : NULL, n, qos,
                                             v3_bool(p, "retain", false), true);
            err = id < 0 ? ESP_FAIL : ESP_OK;
            if (!err) {
                *out = cJSON_CreateObject();
                cJSON_AddNumberToObject(*out, "message_id", id);
                cJSON_AddStringToObject(*out, "delivery", "queued");
            }
        }
        free(bytes);
        return err;
    }
    if (!strcmp(op, "mqtt.subscribe") || !strcmp(op, "mqtt.unsubscribe")) {
        const char *topic = v3_str(p, "topic", "");
        int qos = v3_int(p, "qos", 0);
        if (!*topic || strlen(topic) > 128 || qos < 0 || qos > 2)
            return ESP_ERR_INVALID_ARG;
        int id = !strcmp(op, "mqtt.subscribe") ? esp_mqtt_client_subscribe(mqtt, topic, qos)
                                               : esp_mqtt_client_unsubscribe(mqtt, topic);
        return id < 0 ? ESP_FAIL : ESP_OK;
    }
    if (!strcmp(op, "mqtt.stop"))
        return esp_mqtt_client_stop(mqtt);
    return ESP_ERR_NOT_SUPPORTED;
}
static int mqtt_work(const char *op, const cJSON *p, cJSON **out) {
    if (!mqtt_guard)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mqtt_guard, portMAX_DELAY);
    int err = mqtt_work_impl(op, p, out);
    xSemaphoreGive(mqtt_guard);
    return err;
}
static int espnow_work(const char *op, const cJSON *p, cJSON **out) {
    if (!strcmp(op, "espnow.status")) {
        *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(*out, "started", now_started);
        cJSON_AddNumberToObject(*out, "dropped", atomic_load(&now_dropped));
        cJSON_AddStringToObject(*out, "delivery", "mac_ack_only");
        return ESP_OK;
    }
    if (atomic_load(&maintenance))
        return ESP_ERR_INVALID_STATE;
    if (!strcmp(op, "espnow.start")) {
        int err = ensure_wifi();
        if (err)
            return err;
        if (now_started)
            return ESP_OK;
        if ((err = esp_now_init()))
            return err;
        now_started = true;
        esp_now_register_recv_cb(now_rx);
        esp_now_register_send_cb(now_tx);
        if (v3_get(p, "pmk")) {
            uint8_t key[16];
            size_t n;
            if (!v3_decode64(v3_str(p, "pmk", ""), key, 16, &n) || n != 16)
                return ESP_ERR_INVALID_ARG;
            return esp_now_set_pmk(key);
        }
        return ESP_OK;
    }
    if (!now_started)
        return ESP_ERR_INVALID_STATE;
    if (!strcmp(op, "espnow.stop")) {
        int err = esp_now_deinit();
        if (!err)
            now_started = false;
        return err;
    }
    uint8_t mac[6];
    if (!parse_mac(v3_str(p, "mac", ""), mac))
        return ESP_ERR_INVALID_ARG;
    if (!strcmp(op, "espnow.peer")) {
        esp_now_peer_info_t peer = {
            .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = v3_bool(p, "encrypt", false)};
        memcpy(peer.peer_addr, mac, 6);
        if (peer.encrypt) {
            size_t n;
            if (!v3_decode64(v3_str(p, "key", ""), peer.lmk, 16, &n) || n != 16)
                return ESP_ERR_INVALID_ARG;
        }
        return esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer);
    }
    if (!strcmp(op, "espnow.remove"))
        return esp_now_del_peer(mac);
    if (!strcmp(op, "espnow.send")) {
        uint8_t bytes[250];
        size_t n;
        if (v3_payload(p, bytes, sizeof(bytes), &n))
            return ESP_ERR_INVALID_ARG;
        int timeout = v3_timeout(p);
        if (timeout < 0)
            return ESP_ERR_INVALID_ARG;
        bool expected = false;
        if (!atomic_compare_exchange_strong(&now_pending, &expected, true))
            return ESP_ERR_INVALID_STATE;
        xSemaphoreTake(now_done, 0);
        atomic_store(&now_result, 0);
        int err = esp_now_send(mac, bytes, n);
        if (err) {
            atomic_store(&now_pending, false);
            return err;
        }
        if (xSemaphoreTake(now_done, pdMS_TO_TICKS(timeout)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(*out, "mac_ack", atomic_load(&now_result) == 1);
        cJSON_AddBoolToObject(*out, "application_ack", false);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
int v3_network_work(const v3_work *w, cJSON **out, bool *external) {
    const char *op = w->action;
    const cJSON *p = w->params;
    if (!atomic_load(&network_ready))
        return ESP_ERR_INVALID_STATE;
    if (init_error && strcmp(op, "wifi.status"))
        return init_error;
    if (!strcmp(op, "mqtt.receive")) {
        *out = cJSON_CreateObject();
        cJSON *a = cJSON_AddArrayToObject(*out, "packets");
        net_packet e;
        for (int i = 0; i < 4 && xQueueReceive(telemetry, &e, 0) == pdTRUE; ++i) {
            cJSON *r = NULL;
            int err = v3_binary_result(e.data, e.length, &r);
            if (err)
                return err;
            char *topic = v3_encode64((const uint8_t *)e.topic, strlen(e.topic));
            if (!topic) {
                cJSON_Delete(r);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(r, "topic_base64", topic);
            free(topic);
            cJSON_AddNumberToObject(r, "time_us", (double)e.time);
            v3_push(a, r);
        }
        return ESP_OK;
    }
    if (!strcmp(op, "espnow.receive")) {
        QueueHandle_t queue = now_received;
        v3_packet e;
        *out = cJSON_CreateObject();
        cJSON *a = cJSON_AddArrayToObject(*out, "packets");
        for (int i = 0; i < 8 && xQueueReceive(queue, &e, 0) == pdTRUE; ++i) {
            cJSON *r = NULL;
            v3_binary_result(e.data, e.length, &r);
            cJSON_AddNumberToObject(r, "time_us", (double)e.time);
            v3_add_u32(r, "topic_hash", e.id);
            v3_add_u32(r, "flags", e.flags);
            v3_push(a, r);
        }
        return ESP_OK;
    }
    if (!strncmp(op, "wifi.", 5))
        return wifi_work(op, p, out);
    if (!strcmp(op, "mqtt.publish") || !strcmp(op, "espnow.send"))
        *external = true;
    return LV_HAS_IOT && !strncmp(op, "mqtt.", 5)     ? mqtt_work(op, p, out)
           : LV_HAS_IOT && !strncmp(op, "espnow.", 7) ? espnow_work(op, p, out)
                                                      : ESP_ERR_NOT_SUPPORTED;
}
void v3_network_poll(void) {
    if (reconnect_at && esp_timer_get_time() / 1000 >= reconnect_at && !atomic_load(&maintenance) &&
        wifi_started) {
        reconnect_at = 0;
        esp_wifi_connect();
    }
}

static void restore_network(void) {
    if (v3_nvs_error)
        return;
    nvs_handle_t n;
    if (nvs_open("limivia_wifi", NVS_READONLY, &n) == ESP_OK) {
        wifi_config_t config = {0};
        size_t size = sizeof(config.sta);
        int err = nvs_get_blob(n, "sta", &config.sta, &size);
        nvs_close(n);
        if (!err && size == sizeof(config.sta) && config.sta.ssid[0] && ensure_driver() == ESP_OK &&
            esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK) {
            atomic_store(&wifi_wants_connection, true);
            if (ensure_wifi() == ESP_OK)
                esp_wifi_connect();
        }
    }
    if (LV_HAS_IOT && nvs_open("limivia_mqtt", NVS_READONLY, &n) == ESP_OK) {
        size_t size = 0;
        int err = nvs_get_str(n, "settings", NULL, &size);
        char *text = NULL;
        if (!err && size && size <= LV_REQUEST_MAX + 1) {
            text = malloc(size);
            if (text)
                err = nvs_get_str(n, "settings", text, &size);
        }
        nvs_close(n);
        if (text && !err) {
            cJSON *p = v3_parse(text, size - 1), *out = NULL;
            if (p) {
                cJSON_DeleteItemFromObjectCaseSensitive(p, "persist");
                mqtt_error = mqtt_work("mqtt.configure", p, &out);
                cJSON_Delete(out);
                cJSON_Delete(p);
            }
        }
        free(text);
    }
}
