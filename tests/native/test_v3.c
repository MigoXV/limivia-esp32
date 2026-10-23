#include "v3.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static v3_core core;
static v3_work *queue[64];
static int queued, sequence;
static bool admit = true;
static bool enqueue(void *ctx, v3_work *w) {
    (void)ctx;
    if (!admit || queued == 64)
        return false;
    queue[queued++] = w;
    return true;
}
void setUp(void) {
    v3_init(&core, (v3_adapter){.enqueue = enqueue}, "0123456789abcdef", "aa:bb:cc:dd:ee:ff",
            "test", "bus");
    queued = sequence = 0;
    admit = true;
}
void tearDown(void) {
    for (int i = 0; i < queued; ++i) {
        v3_work_free(queue[i]);
        queue[i] = NULL;
    }
    queued = 0;
    v3_destroy(&core);
    memset(&core, 0, sizeof(core));
}
static cJSON *call_at(int endpoint, uint32_t generation, const char *op, const char *params_text) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", ++sequence);
    cJSON_AddStringToObject(root, "method", op);
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON *context = cJSON_AddObjectToObject(params, "context");
    if (!strcmp(op, "hello")) {
        char nonce[33];
        snprintf(nonce, sizeof(nonce), "%032x", sequence);
        cJSON_AddStringToObject(context, "client_nonce", nonce);
    } else {
        cJSON_AddStringToObject(context, "session", core.sessions[endpoint].token);
        cJSON_AddStringToObject(context, "control", core.control);
        cJSON_AddNumberToObject(context, "seq", sequence);
    }
    cJSON_AddItemToObject(params, "args", cJSON_Parse(params_text));
    char *text = cJSON_PrintUnformatted(root);
    TEST_ASSERT_LESS_OR_EQUAL(LV_REQUEST_MAX, strlen(text));
    cJSON *r = v3_handle(&core, endpoint, generation, text, strlen(text), 1000);
    cJSON_free(text);
    cJSON_Delete(root);
    return r;
}
static cJSON *call(const char *op, const char *p) { return call_at(0, 0, op, p); }
static void ok(cJSON *r) {
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_NULL(v3_get(r, "error"));
    TEST_ASSERT_EQUAL_STRING("2.0", v3_str(r, "jsonrpc", ""));
    cJSON_Delete(r);
}
static void error(cJSON *r, const char *code) {
    TEST_ASSERT_NOT_NULL(v3_get(r, "error"));
    TEST_ASSERT_EQUAL_STRING(code, v3_str(v3_get(v3_get(r, "error"), "data"), "reason", ""));
    cJSON_Delete(r);
}
static void connect_control(void) {
    ok(call("hello", "{}"));
    ok(call("control.acquire", "{}"));
}
static uint32_t open_resource(const char *p) {
    cJSON *r = call("resource.open", p);
    TEST_ASSERT_NULL(v3_get(r, "error"));
    uint32_t id = v3_int(v3_get(v3_get(r, "result"), "value"), "resource", 0);
    TEST_ASSERT_NOT_EQUAL(0, id);
    cJSON_Delete(r);
    return id;
}
static void finish(int index, int err) {
    TEST_ASSERT_LESS_THAN(queued, index);
    v3_work *w = queue[index];
    TEST_ASSERT_NOT_NULL(w);
    v3_complete(&core, w, err, false, NULL);
    v3_work_free(w);
    queue[index] = NULL;
}
static void test_strict_json(void) {
    const char *bad[] = {
        "{\"a\":1,\"a\":2}", "{\"a\":NaN}",         "{\"a\":01}", "{\"a\":1.}",
        "{\"a\":1e999}",     "{\"a\":\"\\u0000\"}", "{}x",        "{\"a\":\"\xc0\xaf\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        TEST_ASSERT_NULL(v3_parse(bad[i], strlen(bad[i])));
    const char text[] = "{\"a\":-1.2e+3,\"b\":\"中文\"}";
    cJSON *p = v3_parse(text, sizeof(text) - 1);
    TEST_ASSERT_NOT_NULL(p);
    cJSON_Delete(p);
    char depth[80];
    memset(depth, '[', 17);
    depth[17] = '0';
    memset(depth + 18, ']', 17);
    TEST_ASSERT_NULL(v3_parse(depth, 35));
    const char raw[] = {'{', '}'};
    p = v3_parse(raw, sizeof(raw));
    TEST_ASSERT_NOT_NULL(p);
    cJSON_Delete(p); // No sentinel read.
}
static void test_versions_and_sessions(void) {
    const char *v2 = "{\"v\":2,\"id\":1,\"op\":\"hello\",\"params\":{}}";
    error(v3_handle(&core, 0, 0, v2, strlen(v2), 1000), "invalid_request");
    error(call("state", "{}"), "invalid_session");
    connect_control();
    char old[V3_TOKEN];
    strcpy(old, core.sessions[0].token);
    v3_disconnect(&core, 0, 1);
    TEST_ASSERT_EQUAL(-1, core.owner);
    TEST_ASSERT_EQUAL_STRING("", core.sessions[0].token);
    ok(call_at(0, 1, "hello", "{}"));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(old, core.sessions[0].token));
    v3_tick(&core, 7000);
    TEST_ASSERT_EQUAL_STRING("", core.sessions[0].token);
}
static void test_pin_protection_and_atomic_preflight(void) {
    connect_control();
    error(call("resource.open", "{\"kind\":\"uart\",\"pins\":{\"tx\":0,\"rx\":19},\"config\":{}}"),
          "reserved_pin");
    TEST_ASSERT_TRUE(v3_idle(&core));
    ok(call("resource.preflight",
            "{\"kind\":\"gpio\",\"pins\":{\"pin\":0},\"config\":{\"mode\":\"output\"}}"));
    TEST_ASSERT_TRUE(v3_idle(&core));
    int protected[] = {2, 8, 9, 11, 12, 14, 15, 16, 17, 18, 19};
    for (size_t i = 0; i < sizeof(protected) / sizeof(*protected); ++i)
        TEST_ASSERT_FALSE(v3_pin_allowed(protected[i]));
    int digital[] = {0, 1, 3, 4, 5, 6, 7, 10, 13, 20, 21};
    for (size_t i = 0; i < sizeof(digital) / sizeof(*digital); ++i)
        TEST_ASSERT_TRUE(v3_pin_allowed(digital[i]));
}
static void test_analog_digital_exclusion(void) {
    connect_control();
    uint32_t id = open_resource("{\"kind\":\"adc\",\"pins\":{\"pin\":0},\"config\":{}}");
    finish(0, 0);
    error(call("resource.open", "{\"kind\":\"gpio\",\"pins\":{\"pin\":0},\"config\":{}}"),
          "pin_conflict");
    error(call("resource.open", "{\"kind\":\"adc\",\"pins\":{\"pin\":5},\"config\":{}}"),
          "adc_unavailable");
    strcpy(core.profile, "signal");
    error(call("resource.open", "{\"kind\":\"adc.continuous\",\"pins\":{\"ch1\":1},\"config\":{}}"),
          "adc_unit_conflict");
    char p[80];
    snprintf(p, sizeof(p), "{\"resource\":%lu}", (unsigned long)id);
    ok(call("resource.close", p));
    finish(1, 0);
    open_resource("{\"kind\":\"gpio\",\"pins\":{\"pin\":0},\"config\":{}}");
    finish(2, 0);
}
static void test_pwm_six_channels_and_shared_timers(void) {
    connect_control();
    int pins[] = {0, 1, 3, 4, 5, 6};
    char p[180];
    for (int i = 0; i < 6; ++i) {
        snprintf(p, sizeof(p),
                 "{\"kind\":\"pwm\",\"pins\":{\"pin\":%d},\"config\":{\"frequency\":1000,"
                 "\"resolution\":10}}",
                 pins[i]);
        open_resource(p);
        finish(i, 0);
    }
    for (int i = 0; i < 6; ++i)
        TEST_ASSERT_EQUAL(core.resources[0].timer, core.resources[i].timer);
    error(call("resource.open", "{\"kind\":\"pwm\",\"pins\":{\"pin\":7},\"config\":{}}"),
          "controller_conflict");
}
static void test_pwm_four_frequencies(void) {
    connect_control();
    int pins[] = {0, 1, 3, 4};
    char p[180];
    for (int i = 0; i < 4; ++i) {
        snprintf(p, sizeof(p),
                 "{\"kind\":\"pwm\",\"pins\":{\"pin\":%d},\"config\":{\"frequency\":%d}}", pins[i],
                 1000 + i * 1000);
        open_resource(p);
        finish(i, 0);
    }
    error(call("resource.open",
               "{\"kind\":\"pwm\",\"pins\":{\"pin\":5},\"config\":{\"frequency\":5000}}"),
          "pwm_timer_conflict");
}
static void test_controller_roles_and_profiles(void) {
    connect_control();
    open_resource("{\"kind\":\"i2c.master\",\"pins\":{\"sda\":0,\"scl\":1},\"config\":{}}");
    finish(0, 0);
    error(
        call("resource.open",
             "{\"kind\":\"i2c.slave\",\"pins\":{\"sda\":3,\"scl\":4},\"config\":{\"address\":42}}"),
        "controller_conflict");
    error(call("resource.open",
               "{\"kind\":\"i2c.master\",\"pins\":{\"sda\":3,\"scl\":3},\"config\":{}}"),
          "duplicate_pin");
    strcpy(core.profile, "signal");
    error(call("resource.open", "{\"kind\":\"uart\",\"pins\":{\"tx\":3},\"config\":{}}"),
          "feature_unavailable");
    strcpy(core.profile, "iot");
    error(call("resource.open",
               "{\"kind\":\"spi.slave\",\"pins\":{\"sclk\":3,\"cs\":4},\"config\":{}}"),
          "feature_unavailable");
}
static void test_parent_dependency_and_cascade(void) {
    connect_control();
    uint32_t parent =
        open_resource("{\"kind\":\"i2c.master\",\"pins\":{\"sda\":0,\"scl\":1},\"config\":{}}");
    finish(0, 0);
    char p[220];
    snprintf(p, sizeof(p),
             "{\"kind\":\"i2c.device\",\"parent\":%lu,\"pins\":{},\"config\":{\"address\":42}}",
             (unsigned long)parent);
    uint32_t child = open_resource(p);
    finish(1, 0);
    snprintf(p, sizeof(p), "{\"resource\":%lu}", (unsigned long)parent);
    error(call("resource.close", p), "children_exist");
    snprintf(p, sizeof(p), "{\"resource\":%lu,\"cascade\":true}", (unsigned long)parent);
    ok(call("resource.close", p));
    v3_tick(&core, 1000);
    TEST_ASSERT_EQUAL(child, queue[2]->resource);
    finish(2, 0);
    v3_tick(&core, 1000);
    TEST_ASSERT_EQUAL(parent, queue[3]->resource);
    finish(3, 0);
    TEST_ASSERT_TRUE(v3_idle(&core));
}
static void test_stop_stale_open_and_failed_cleanup(void) {
    connect_control();
    uint32_t id =
        open_resource("{\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{\"mode\":\"output\"}}");
    ok(call("outputs.stop", "{}"));
    TEST_ASSERT_EQUAL(-1, core.owner);
    TEST_ASSERT_TRUE(core.autostart_inhibited);
    finish(0, 0);
    TEST_ASSERT_EQUAL(V_CLOSING, v3_find(&core, id)->state);
    v3_tick(&core, 1000);
    finish(1, 123);
    TEST_ASSERT_EQUAL(V_FAULT, v3_find(&core, id)->state);
    TEST_ASSERT_EQUAL(123, v3_find(&core, id)->cleanup_error);
    v3_tick(&core, 1000);
    TEST_ASSERT_FALSE(core.stopping);
    ok(call("control.acquire", "{}"));
    error(call("resource.open", "{\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{}}"),
          "pin_conflict");
    char p[80];
    snprintf(p, sizeof(p), "{\"resource\":%lu}", (unsigned long)id);
    ok(call("resource.recover", p));
    finish(2, 0);
    TEST_ASSERT_TRUE(v3_idle(&core));
}
static void test_stop_has_reserved_job(void) {
    connect_control();
    for (int i = 0; i < V3_JOBS - 1; ++i)
        ok(call("system.info", "{}"));
    error(call("system.info", "{}"), "busy");
    ok(call("outputs.stop", "{}"));
    TEST_ASSERT_TRUE(core.stopping);
}
static void test_base64_canonical_and_atomic_write(void) {
    uint8_t b[20];
    size_t n;
    TEST_ASSERT_TRUE(v3_decode64("AAEC/w==", b, sizeof(b), &n));
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(255, b[3]);
    const char *bad[] = {"A", "AB==", "AA=+", "AAA=AAAA", "AA A", "====", "AA-_"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        TEST_ASSERT_FALSE(v3_decode64(bad[i], b, sizeof(b), &n));
    connect_control();
    cJSON *r = call("buffer.create", "{\"size\":8}");
    int id = v3_int(v3_get(v3_get(r, "result"), "value"), "buffer_id", 0);
    cJSON_Delete(r);
    char p[150];
    snprintf(p, sizeof(p), "{\"buffer_id\":%d,\"offset\":0,\"data\":\"AQID\"}", id);
    ok(call("buffer.write", p));
    snprintf(p, sizeof(p), "{\"buffer_id\":%d,\"offset\":0,\"data\":\"AAA=AAAA\"}", id);
    error(call("buffer.write", p), "invalid_base64_or_range");
    TEST_ASSERT_EQUAL(1, core.buffers[0].data[0]);
}
static void test_job_payload_ownership(void) {
    connect_control();
    uint32_t id = open_resource("{\"kind\":\"uart\",\"pins\":{\"tx\":20,\"rx\":21},\"config\":{}}");
    finish(0, 0);
    char p[180];
    snprintf(p, sizeof(p), "{\"resource\":%lu,\"action\":\"write\",\"args\":{\"data\":\"AQID\"}}",
             (unsigned long)id);
    ok(call("resource.call", p));
    TEST_ASSERT_EQUAL_STRING("AQID", v3_str(queue[1]->params, "data", ""));
    TEST_ASSERT_EQUAL_STRING("uart", v3_str(queue[1]->definition, "kind", ""));
}
static void test_capabilities_frames_and_revision(void) {
    connect_control();
    char bytes[LV_RESPONSE_MAX + 8];
    const char *profiles[] = {"bus", "signal", "iot"};
    for (int p = 0; p < 3; ++p) {
        strcpy(core.profile, profiles[p]);
        for (int page = 0; page < v3_capability_pages(); ++page) {
            cJSON *r = v3_capabilities(&core, page);
            size_t n = v3_frame(r, bytes, sizeof(bytes));
            TEST_ASSERT_GREATER_THAN(0, n);
            TEST_ASSERT_LESS_OR_EQUAL(LV_RESPONSE_MAX + 1, n);
            cJSON_Delete(r);
        }
    }
    core.revision = 4;
    error(call("state", "{\"page\":1,\"revision\":3}"), "snapshot_expired");
}
static uint32_t install_flow(const char *text) {
    cJSON *def = cJSON_Parse(text);
    uint32_t id = 0;
    const char *error = v3_flow_install(&core, "test.flow.json", def, &id);
    cJSON_Delete(def);
    TEST_ASSERT_NULL(error);
    return id;
}
static cJSON *flow_call(const char *op, uint32_t id) {
    char p[64];
    snprintf(p, sizeof(p), "{\"flow_id\":%lu}", (unsigned long)id);
    return call(op, p);
}
static void test_flow_rejects_busy_cycles(void) {
    cJSON *def = cJSON_Parse(
        "{\"version\":1,\"profile\":\"bus\",\"resources\":[],\"variables\":{\"x\":0},\"steps\":[{"
        "\"op\":\"if\",\"variable\":\"x\",\"value\":0,\"compare\":\"eq\",\"then\":0,\"else\":1}]}");
    uint32_t id;
    TEST_ASSERT_EQUAL_STRING("busy_loop", v3_flow_install(&core, "bad.flow.json", def, &id));
    cJSON_Delete(def);
    install_flow("{\"version\":1,\"profile\":\"bus\",\"resources\":[],\"variables\":{\"x\":0},"
                 "\"steps\":[{\"op\":\"delay\",\"ms\":10},{\"op\":\"if\",\"variable\":\"x\","
                 "\"value\":0,\"compare\":\"eq\",\"then\":0,\"else\":2}]}");
}
static void test_flow_preflight_is_atomic(void) {
    connect_control();
    uint32_t id =
        install_flow("{\"version\":1,\"profile\":\"bus\",\"resources\":[{\"name\":\"a\",\"kind\":"
                     "\"gpio\",\"pins\":{\"pin\":0},\"config\":{}},{\"name\":\"b\",\"kind\":"
                     "\"gpio\",\"pins\":{\"pin\":0},\"config\":{}}],\"steps\":[{\"op\":\"end\"}]}");
    error(flow_call("flow.start", id), "pin_conflict");
    TEST_ASSERT_TRUE(v3_idle(&core));
    TEST_ASSERT_EQUAL(0, queued);
}
static void test_flow_owns_resources_and_continues_offline(void) {
    connect_control();
    uint32_t id = install_flow(
        "{\"version\":1,\"profile\":\"bus\",\"resources\":[{\"name\":\"led\",\"kind\":\"gpio\","
        "\"pins\":{\"pin\":13},\"config\":{\"mode\":\"output\"}}],\"steps\":[{\"op\":\"delay\","
        "\"ms\":100},{\"op\":\"call\",\"resource\":\"led\",\"action\":\"write\",\"args\":{"
        "\"level\":1},\"timeout_ms\":1000},{\"op\":\"end\"}]}");
    ok(flow_call("flow.start", id));
    v3_tick(&core, 1000);
    finish(0, 0);
    v3_tick(&core, 1001);
    v3_tick(&core, 1002);
    char p[160];
    snprintf(p, sizeof(p), "{\"resource\":%lu,\"action\":\"write\",\"args\":{\"level\":0}}",
             (unsigned long)core.resources[0].id);
    error(call("resource.call", p), "resource_busy");
    v3_tick(&core, 1003);
    v3_tick(&core, 1010);
    TEST_ASSERT_EQUAL(1, queued);
    v3_disconnect(&core, 0, 1);
    v3_tick(&core, 1110);
    TEST_ASSERT_EQUAL(2, queued);
    TEST_ASSERT_EQUAL_STRING("write", queue[1]->action);
    finish(1, 0);
    v3_tick(&core, 1111);
    v3_tick(&core, 1112);
    v3_tick(&core, 1113);
    cJSON *params = cJSON_CreateObject();
    v3_add_u32(params, "flow_id", id);
    const char *err = NULL;
    cJSON *state = v3_flow_operation(&core, "flow.get", params, &err);
    TEST_ASSERT_EQUAL_STRING("completed", v3_str(state, "state", ""));
    cJSON_Delete(state);
    cJSON_Delete(params);
    TEST_ASSERT_FALSE(v3_idle(&core)); // Completion preserves outputs, stop clears them.
}
static void test_flow_stop_during_wait_prevents_future_calls(void) {
    connect_control();
    uint32_t id = install_flow(
        "{\"version\":1,\"profile\":\"bus\",\"autostart\":true,\"resources\":[{\"name\":\"led\","
        "\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{\"mode\":\"output\"}}],\"steps\":[{"
        "\"op\":\"delay\",\"ms\":1000},{\"op\":\"call\",\"resource\":\"led\",\"action\":\"write\","
        "\"args\":{\"level\":1},\"timeout_ms\":100}]}");
    ok(flow_call("flow.start", id));
    v3_tick(&core, 1000);
    finish(0, 0);
    v3_tick(&core, 1001);
    v3_tick(&core, 1002);
    v3_tick(&core, 1003);
    ok(call("outputs.stop", "{}"));
    v3_tick(&core, 1100);
    TEST_ASSERT_EQUAL_STRING("close", queue[1]->action);
    finish(1, 0);
    v3_tick(&core, 3000);
    TEST_ASSERT_EQUAL(2, queued);
    TEST_ASSERT_TRUE(core.autostart_inhibited);
    TEST_ASSERT_TRUE(v3_idle(&core));
}
static void test_cancelled_open_cannot_rearm_after_flow_stop(void) {
    connect_control();
    uint32_t id = install_flow(
        "{\"version\":1,\"profile\":\"bus\",\"resources\":[{\"name\":\"led\",\"kind\":\"gpio\","
        "\"pins\":{\"pin\":13},\"config\":{\"mode\":\"output\"}}],\"steps\":[{\"op\":\"end\"}]}");
    ok(flow_call("flow.start", id));
    v3_tick(&core, 1000);
    TEST_ASSERT_EQUAL(1, queued);
    ok(flow_call("flow.stop", id));
    TEST_ASSERT_TRUE(atomic_load(&queue[0]->cancelled));
    finish(0, 0);
    TEST_ASSERT_EQUAL(V_CLOSING, core.resources[0].state);
    v3_tick(&core, 1001);
    finish(1, 0);
    v3_tick(&core, 1002);
    TEST_ASSERT_TRUE(v3_idle(&core));
}
static void test_parent_cleanup_fault_completes_stop_as_failed(void) {
    connect_control();
    uint32_t parent =
        open_resource("{\"kind\":\"i2c.master\",\"pins\":{\"sda\":0,\"scl\":1},\"config\":{}}");
    finish(0, 0);
    char p[180];
    snprintf(p, sizeof(p),
             "{\"kind\":\"i2c.device\",\"parent\":%lu,\"pins\":{},\"config\":{\"address\":80}}",
             (unsigned long)parent);
    open_resource(p);
    finish(1, 0);
    ok(call("outputs.stop", "{}"));
    v3_tick(&core, 1000);
    finish(2, 123);
    v3_tick(&core, 1001);
    v3_tick(&core, 1002);
    TEST_ASSERT_FALSE(core.stopping);
    TEST_ASSERT_EQUAL(V_FAULT, v3_find(&core, parent)->state);
    TEST_ASSERT_EQUAL(J_FAILED, core.jobs[V3_JOBS - 1].state);
}
static void test_bounded_result_history_is_explicitly_expired(void) {
    connect_control();
    char data[5465];
    memset(data, 'A', sizeof(data) - 1);
    data[sizeof(data) - 1] = 0;
    for (int i = 0; i < 20; ++i) {
        ok(call("system.info", "{}"));
        v3_work *w = queue[queued - 1];
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "data", data);
        v3_complete(&core, w, 0, false, result);
        v3_work_free(w);
        queue[queued - 1] = NULL;
        TEST_ASSERT_LESS_OR_EQUAL(32768, core.retained_result_bytes);
    }
    size_t total = 0;
    bool expired = false;
    for (int i = 0; i < V3_JOBS; ++i) {
        total += core.jobs[i].result_bytes;
        expired |= core.jobs[i].result_expired;
    }
    TEST_ASSERT_EQUAL(total, core.retained_result_bytes);
    TEST_ASSERT_TRUE(expired);
}
static void test_admission_failure_preserves_all_pin_claims(void) {
    connect_control();
    admit = false;
    error(call("resource.open", "{\"kind\":\"uart\",\"pins\":{\"tx\":20,\"rx\":21},\"config\":{}}"),
          "busy");
    TEST_ASSERT_TRUE(v3_idle(&core));
    TEST_ASSERT_EQUAL(0, queued);
}
static void test_planned_sleep_restores_next_step_and_variables(void) {
    strcpy(core.profile, "iot");
    connect_control();
    const char *text =
        "{\"version\":1,\"profile\":\"iot\",\"resources\":[],\"variables\":{\"x\":0},\"steps\":[{"
        "\"op\":\"set\",\"variable\":\"x\",\"value\":7},{\"op\":\"sleep\",\"duration_ms\":1000},{"
        "\"op\":\"add\",\"variable\":\"x\",\"value\":1},{\"op\":\"end\"}]}";
    uint32_t id = install_flow(text);
    ok(flow_call("flow.start", id));
    for (int i = 0; i < 4; ++i)
        v3_tick(&core, 1000 + i);
    TEST_ASSERT_EQUAL(1, queued);
    TEST_ASSERT_EQUAL_STRING("power.sleep", queue[0]->action);
    cJSON *checkpoint = cJSON_Duplicate(v3_get(queue[0]->params, "checkpoint"), true);
    TEST_ASSERT_NOT_NULL(checkpoint);
    TEST_ASSERT_EQUAL(2, v3_int(checkpoint, "pc", -1));
    tearDown();
    setUp();
    strcpy(core.profile, "iot");
    id = install_flow(text);
    TEST_ASSERT_NULL(v3_flow_restore(&core, id, checkpoint));
    cJSON_Delete(checkpoint);
    connect_control();
    ok(flow_call("flow.start", id));
    for (int i = 0; i < 4; ++i)
        v3_tick(&core, 1000 + i);
    cJSON *state = flow_call("flow.get", id);
    const cJSON *flow = v3_get(v3_get(state, "result"), "value");
    TEST_ASSERT_EQUAL_STRING("completed", v3_str(flow, "state", ""));
    TEST_ASSERT_EQUAL(8, v3_int(v3_get(flow, "variables"), "x", 0));
    TEST_ASSERT_EQUAL(0, queued);
    cJSON_Delete(state);
}
static void test_notification_subscription_is_session_scoped(void) {
    ok(call("hello", "{}"));
    ok(call("event.subscribe", "{\"topics\":[\"job\",\"data\"],\"interval_ms\":100}"));
    TEST_ASSERT_EQUAL(9, core.sessions[0].event_mask);
    ok(call("hello", "{}"));
    TEST_ASSERT_EQUAL(0, core.sessions[0].event_mask);
    error(call("event.subscribe", "{\"topics\":[\"unknown\"]}"), "invalid_argument");
}
static void test_retained_mqtt_and_duplicate_writes_never_execute(void) {
    ok(call_at(LV_MQTT0, 0, "hello", "{}"));
    ok(call_at(LV_MQTT0, 0, "control.acquire", "{}"));
    char text[512];
    int n = snprintf(
        text, sizeof(text),
        "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"resource.open\",\"params\":{"
        "\"context\":{\"seq\":100,\"session\":\"%s\",\"control\":\"%s\"},"
        "\"args\":{\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{\"mode\":\"output\"}}}}",
        core.sessions[LV_MQTT0].token, core.control);
    error(v3_handle_message(&core, LV_MQTT0, 0, text, n, 1000, true), "retained_command");
    TEST_ASSERT_EQUAL(0, queued);
    TEST_ASSERT_TRUE(v3_idle(&core));
    ok(v3_handle_message(&core, LV_MQTT0, 0, text, n, 1000, false));
    TEST_ASSERT_EQUAL(1, queued);
    error(v3_handle_message(&core, LV_MQTT0, 0, text, n, 1001, false), "duplicate_request");
    TEST_ASSERT_EQUAL(1, queued);
}
typedef union {
    max_align_t alignment;
    size_t bytes;
} allocation_header;
static int allocation_calls, allocation_fail, allocation_live;
static void *failing_alloc(size_t n) {
    if (++allocation_calls == allocation_fail)
        return NULL;
    allocation_header *p = malloc(sizeof(*p) + n);
    if (!p)
        return NULL;
    ++allocation_live;
    return p + 1;
}
static void failing_free(void *p) {
    if (p) {
        --allocation_live;
        free((allocation_header *)p - 1);
    }
}
static void test_every_json_allocation_failure_releases_owned_data(void) {
    v3_destroy(&core);
    int sites = 1;
    cJSON_Hooks hooks = {.malloc_fn = failing_alloc, .free_fn = failing_free};
    cJSON_InitHooks(&hooks);
    for (int fail = 0; fail <= sites; ++fail) {
        setUp();
        allocation_fail = 0;
        connect_control();
        char request[512];
        int n = snprintf(request, sizeof(request),
                         "{\"jsonrpc\":\"2.0\",\"id\":1000000,\"method\":\"resource.open\",\"params\":{"
                         "\"context\":{\"seq\":1000000,\"session\":\"%s\",\"control\":\"%s\"},"
                         "\"args\":{\"kind\":\"gpio\",\"pins\":{\"pin\":13},"
                         "\"config\":{\"mode\":\"output\",\"level\":1}}}}",
                         core.sessions[0].token, core.control);
        allocation_calls = 0;
        allocation_fail = fail;
        cJSON *reply = v3_handle(&core, 0, 0, request, n, 1000);
        if (!fail)
            sites = allocation_calls;
        allocation_fail = 0;
        cJSON_Delete(reply);
        tearDown();
        TEST_ASSERT_EQUAL_MESSAGE(0, allocation_live,
                                  "A failed JSON allocation leaked an owned request/resource/job");
    }
    cJSON_InitHooks(NULL);
    setUp();
}
static void test_discovery_survives_every_json_allocation_failure(void) {
    cJSON_Hooks hooks = {.malloc_fn = failing_alloc, .free_fn = failing_free};
    cJSON_InitHooks(&hooks);
    for (int page = 0; page < v3_capability_pages(); ++page) {
        int sites = 1;
        for (int fail = 0; fail <= sites; ++fail) {
            allocation_calls = 0;
            allocation_fail = fail;
            cJSON *p = v3_capabilities(&core, page);
            if (!fail)
                sites = allocation_calls;
            allocation_fail = 0;
            cJSON_Delete(p);
            TEST_ASSERT_EQUAL(0, allocation_live);
        }
    }
    cJSON_InitHooks(NULL);
}

static char rpc_output[LV_RESPONSE_MAX + 8];
static size_t wire(const char *text) {
    memset(rpc_output, 0, sizeof(rpc_output));
    return v3_rpc_wire(&core, 0, core.sessions[0].generation, text, strlen(text),
                       1000, false, rpc_output, sizeof(rpc_output));
}
static cJSON *request_json(int seq, const char *method, const char *args) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(p, "id", seq);
    cJSON_AddStringToObject(p, "method", method);
    cJSON *params = cJSON_AddObjectToObject(p, "params");
    cJSON *ctx = cJSON_AddObjectToObject(params, "context");
    cJSON_AddStringToObject(ctx, "session", core.sessions[0].token);
    cJSON_AddStringToObject(ctx, "control", core.control);
    cJSON_AddNumberToObject(ctx, "seq", seq);
    cJSON_AddItemToObject(params, "args", cJSON_Parse(args));
    return p;
}
static cJSON *send_json(cJSON *p) {
    char *text = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    TEST_ASSERT_LESS_OR_EQUAL(LV_REQUEST_MAX, strlen(text));
    size_t size = wire(text);
    cJSON_free(text);
    return size ? v3_parse(rpc_output, size) : NULL;
}
static int error_number(const cJSON *p) { return v3_int(v3_get(p, "error"), "code", 0); }
static void test_rpc_ids_positional_and_standard_errors(void) {
    connect_control();
    const char *ids[] = {"null", "\"uuid-abc\"", "9007199254740993", "-1.25", "1e2",
                         "\"escaped\\\"\\u4e2d\""};
    for (int i = 0; i < 6; ++i) {
        char text[512], expected[100];
        snprintf(text, sizeof(text),
                 "{\"jsonrpc\":\"2.0\",\"i\\u0064\":%s,\"method\":\"state\","
                 "\"params\":[{\"session\":\"%s\",\"seq\":%d},{\"page\":0}]}",
                 ids[i], core.sessions[0].token, i + 10);
        TEST_ASSERT_GREATER_THAN(0, wire(text));
        snprintf(expected, sizeof(expected), "\"id\":%s,", ids[i]);
        TEST_ASSERT_NOT_NULL(strstr(rpc_output, expected));
        cJSON *p = cJSON_Parse(rpc_output);
        TEST_ASSERT_EQUAL(0, error_number(p));
        cJSON_Delete(p);
    }
    TEST_ASSERT_GREATER_THAN(0, wire("{bad"));
    cJSON *p = cJSON_Parse(rpc_output);
    TEST_ASSERT_EQUAL(-32700, error_number(p));
    TEST_ASSERT_TRUE(cJSON_IsNull(v3_get(p, "id")));
    cJSON_Delete(p);
    TEST_ASSERT_GREATER_THAN(0, wire("{\"jsonrpc\":\"2.0\",\"method\":1,\"id\":false}"));
    p = cJSON_Parse(rpc_output);
    TEST_ASSERT_EQUAL(-32600, error_number(p));
    cJSON_Delete(p);
    TEST_ASSERT_GREATER_THAN(0, wire("{\"jsonrpc\":\"2.0\",\"method\":\"foobar\",\"id\":\"1\"}"));
    p = cJSON_Parse(rpc_output);
    TEST_ASSERT_EQUAL(-32601, error_number(p));
    cJSON_Delete(p);
    TEST_ASSERT_EQUAL(0, wire("{\"jsonrpc\":\"2.0\",\"method\":\"foobar\"}"));
    p = send_json(request_json(100, "state", "{\"unknown\":true}"));
    TEST_ASSERT_EQUAL(-32602, error_number(p));
    cJSON_Delete(p);
}
static void test_rpc_nonce_and_sequence_are_independent_of_id(void) {
    connect_control();
    char token[V3_TOKEN], control[V3_TOKEN];
    strcpy(token, core.sessions[0].token);
    strcpy(control, core.control);
    char hello_text[256];
    snprintf(hello_text, sizeof(hello_text),
             "{\"jsonrpc\":\"2.0\",\"id\":\"retry\",\"method\":\"hello\","
             "\"params\":{\"context\":{\"client_nonce\":\"%s\"}}}", core.sessions[0].client_nonce);
    TEST_ASSERT_GREATER_THAN(0, wire(hello_text));
    TEST_ASSERT_EQUAL_STRING(token, core.sessions[0].token);
    TEST_ASSERT_EQUAL_STRING(control, core.control);
    ok(send_json(request_json(100, "session.ping", "{}")));
    ok(send_json(request_json(99, "session.ping", "{}")));
    error(send_json(request_json(99, "session.ping", "{}")), "duplicate_request");
    error(send_json(request_json(36, "session.ping", "{}")), "duplicate_request");
    cJSON *p = request_json(101, "resource.open",
                           "{\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{}}");
    char *text = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    TEST_ASSERT_GREATER_THAN(0, wire(text));
    TEST_ASSERT_EQUAL(1, queued);
    TEST_ASSERT_GREATER_THAN(0, wire(text));
    p = cJSON_Parse(rpc_output);
    TEST_ASSERT_EQUAL(1002, error_number(p));
    TEST_ASSERT_EQUAL(1, queued);
    cJSON_Delete(p);
    cJSON_free(text);
}
static void test_rpc_mixed_batch_and_limits(void) {
    connect_control();
    cJSON *batch = cJSON_CreateArray();
    v3_push(batch, cJSON_CreateNumber(3));
    cJSON *notify = request_json(20, "session.ping", "{}");
    cJSON_DeleteItemFromObjectCaseSensitive(notify, "id");
    v3_push(batch, notify);
    v3_push(batch, request_json(21, "state", "{\"page\":0}"));
    cJSON *p = send_json(batch);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(p));
    TEST_ASSERT_EQUAL(-32600, error_number(cJSON_GetArrayItem(p, 0)));
    TEST_ASSERT_NOT_NULL(v3_get(cJSON_GetArrayItem(p, 1), "result"));
    cJSON_Delete(p);
    TEST_ASSERT_GREATER_THAN(0, wire("[]"));
    p = cJSON_Parse(rpc_output);
    TEST_ASSERT_EQUAL(-32600, error_number(p));
    cJSON_Delete(p);
    TEST_ASSERT_EQUAL(0, wire("[0,0,0,0,0,0,0,0,0]"));
    TEST_ASSERT_EQUAL(-1, v3_rpc_priority("[0,0,0,0,0,0,0,0,0]", 19));
    batch = cJSON_CreateArray();
    notify = request_json(22, "session.ping", "{}");
    cJSON_DeleteItemFromObjectCaseSensitive(notify, "id");
    v3_push(batch, notify);
    TEST_ASSERT_NULL(send_json(batch));
}
static void test_rpc_batch_stop_barrier_at_every_position(void) {
    for (int index = 0; index < 3; ++index) {
        connect_control();
        cJSON *batch = cJSON_CreateArray();
        int base = sequence + 100;
        for (int i = 0; i < 3; ++i)
            v3_push(batch, request_json(base + i, i == index ? "outputs.stop" : "resource.open",
                                        i == index ? "{}" :
                                        "{\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{}}"));
        cJSON *p = send_json(batch);
        TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(p));
        TEST_ASSERT_EQUAL(base + index, v3_int(cJSON_GetArrayItem(p, 0), "id", 0));
        TEST_ASSERT_NOT_NULL(v3_get(cJSON_GetArrayItem(p, 0), "result"));
        TEST_ASSERT_EQUAL(1000, error_number(cJSON_GetArrayItem(p, 1)));
        TEST_ASSERT_EQUAL(1000, error_number(cJSON_GetArrayItem(p, 2)));
        TEST_ASSERT_EQUAL(0, queued);
        TEST_ASSERT_TRUE(v3_idle(&core));
        cJSON_Delete(p);
        sequence = base + 3;
    }
}
static int interrupts;
static void injected_stop(void *ctx) {
    (void)ctx;
    if (++interrupts == 1) {
        const char *error = NULL;
        cJSON *p = cJSON_Parse("{}");
        cJSON *result = v3_dispatch(&core, 0, "outputs.stop", p, "", &error);
        cJSON_Delete(result);
        cJSON_Delete(p);
        TEST_ASSERT_NULL(error);
    }
}
static void test_rpc_new_stop_interrupts_batch_between_items(void) {
    connect_control();
    core.adapter.interrupt = injected_stop;
    interrupts = 0;
    cJSON *batch = cJSON_CreateArray();
    v3_push(batch, request_json(20, "resource.open",
                               "{\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{}}"));
    v3_push(batch, request_json(21, "control.acquire", "{}"));
    v3_push(batch, request_json(22, "resource.open",
                               "{\"kind\":\"gpio\",\"pins\":{\"pin\":0},\"config\":{}}"));
    cJSON *p = send_json(batch);
    TEST_ASSERT_EQUAL(1, queued);
    TEST_ASSERT_NOT_NULL(v3_get(cJSON_GetArrayItem(p, 0), "result"));
    for (int i = 1; i < 3; ++i)
        TEST_ASSERT_EQUAL_STRING("stop_in_progress",
            v3_str(v3_get(v3_get(cJSON_GetArrayItem(p, i), "error"), "data"), "reason", ""));
    TEST_ASSERT_EQUAL(-1, core.owner);
    TEST_ASSERT_TRUE(core.stopping);
    cJSON_Delete(p);
}
static int diagnostic_calls;
static cJSON *large_diagnostics(void *ctx, const char *method, const cJSON *args,
                                const char **error) {
    (void)ctx; (void)method; (void)args; (void)error;
    ++diagnostic_calls;
    char text[7001];
    memset(text, 'x', sizeof(text) - 1);
    text[7000] = 0;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "large", text);
    return p;
}
static void test_rpc_growing_output_never_redispatches_the_operation(void) {
    connect_control();
    core.adapter.service = large_diagnostics;
    diagnostic_calls = 0;
    cJSON *p = request_json(20, "diagnostics", "{}");
    char *request = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    size_t size = 0;
    char *out = v3_rpc_alloc(&core, 0, 0, request, strlen(request), 1000, false, &size);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_GREATER_THAN(7000, size);
    TEST_ASSERT_LESS_OR_EQUAL(LV_RESPONSE_MAX + 1, size);
    TEST_ASSERT_EQUAL(1, diagnostic_calls);
    p = cJSON_Parse(out);
    TEST_ASSERT_NOT_NULL(v3_get(p, "result"));
    cJSON_Delete(p);
    free(out);
    cJSON_free(request);
    p = request_json(21, "diagnostics", "{}");
    request = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    char tiny[512];
    size = v3_rpc_wire(&core, 0, 0, request, strlen(request), 1000, false, tiny, sizeof(tiny));
    TEST_ASSERT_GREATER_THAN(0, size);
    TEST_ASSERT_EQUAL(2, diagnostic_calls);
    p = cJSON_Parse(tiny);
    TEST_ASSERT_EQUAL(1001, error_number(p));
    cJSON_Delete(p);
    cJSON_free(request);
}
static void test_rpc_batch_response_budget_and_notice(void) {
    connect_control();
    core.adapter.service = large_diagnostics;
    cJSON *batch = cJSON_CreateArray();
    for (int i = 0; i < 4; ++i)
        v3_push(batch, request_json(20 + i, "diagnostics", "{}"));
    cJSON *p = send_json(batch);
    TEST_ASSERT_EQUAL(4, cJSON_GetArraySize(p));
    TEST_ASSERT_LESS_OR_EQUAL(LV_RESPONSE_MAX + 1, strlen(rpc_output));
    TEST_ASSERT_NOT_NULL(v3_get(cJSON_GetArrayItem(p, 0), "result"));
    TEST_ASSERT_EQUAL(1001, error_number(cJSON_GetArrayItem(p, 1)));
    cJSON_Delete(p);
    p = v3_rpc_notice(&core, 0, "job.finished", cJSON_CreateObject());
    TEST_ASSERT_NULL(v3_get(p, "id"));
    TEST_ASSERT_EQUAL_STRING("2.0", v3_str(p, "jsonrpc", ""));
    TEST_ASSERT_EQUAL_STRING(core.sessions[0].token,
                             v3_str(v3_get(v3_get(p, "params"), "context"), "session", ""));
    cJSON_Delete(p);
}
static void test_rpc_notifications_still_execute_stop_and_guard_sequences(void) {
    connect_control();
    cJSON *p = request_json(20, "outputs.stop", "{}");
    cJSON_DeleteItemFromObjectCaseSensitive(p, "id");
    TEST_ASSERT_NULL(send_json(p));
    TEST_ASSERT_TRUE(core.autostart_inhibited);
    TEST_ASSERT_EQUAL(-1, core.owner);
    error(send_json(request_json(20, "outputs.stop", "{}")), "duplicate_request");
    cJSON *batch = cJSON_CreateArray();
    v3_push(batch, request_json(21, "outputs.stop", "{\"invalid\":true}"));
    v3_push(batch, request_json(22, "control.acquire", "{}"));
    p = send_json(batch);
    TEST_ASSERT_EQUAL(-32602, error_number(cJSON_GetArrayItem(p, 0)));
    TEST_ASSERT_NOT_NULL(v3_get(cJSON_GetArrayItem(p, 1), "result"));
    cJSON_Delete(p);
}
static void test_rpc_batch_and_notice_allocation_failures(void) {
    v3_destroy(&core);
    cJSON_Hooks hooks = {.malloc_fn = failing_alloc, .free_fn = failing_free};
    cJSON_InitHooks(&hooks);
    int sites = 1;
    for (int fail = 0; fail <= sites; ++fail) {
        setUp();
        allocation_fail = 0;
        connect_control();
        cJSON *batch = cJSON_CreateArray();
        v3_push(batch, request_json(30, "resource.open",
                                   "{\"kind\":\"gpio\",\"pins\":{\"pin\":13},\"config\":{}}"));
        v3_push(batch, request_json(31, "outputs.stop", "{}"));
        v3_push(batch, request_json(32, "state", "{\"page\":0}"));
        char *text = cJSON_PrintUnformatted(batch);
        cJSON_Delete(batch);
        allocation_calls = 0;
        allocation_fail = fail;
        size_t size = wire(text);
        if (!fail)
            sites = allocation_calls;
        allocation_fail = 0;
        TEST_ASSERT_GREATER_THAN(0, size);
        cJSON *p = cJSON_Parse(rpc_output);
        TEST_ASSERT_NOT_NULL(p);
        const cJSON *item = cJSON_IsArray(p) ? p->child : p;
        for (; item; item = cJSON_IsArray(p) ? item->next : NULL) {
            TEST_ASSERT_EQUAL_STRING("2.0", v3_str(item, "jsonrpc", ""));
            TEST_ASSERT_NOT_NULL(v3_get(item, "id"));
            TEST_ASSERT_TRUE(!!v3_get(item, "error") != !!v3_get(item, "result"));
        }
        cJSON_Delete(p);
        cJSON_free(text);
        tearDown();
        TEST_ASSERT_EQUAL_MESSAGE(0, allocation_live, "batch OOM leaked JSON ownership");
    }
    sites = 1;
    for (int fail = 0; fail <= sites; ++fail) {
        cJSON *args = cJSON_CreateObject();
        allocation_calls = 0;
        allocation_fail = fail;
        cJSON *p = v3_rpc_notice(&core, 0, "state.changed", args);
        if (!fail)
            sites = allocation_calls;
        allocation_fail = 0;
        cJSON_Delete(p);
        TEST_ASSERT_EQUAL(0, allocation_live);
    }
    cJSON_InitHooks(NULL);
    setUp();
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_strict_json);
    RUN_TEST(test_versions_and_sessions);
    RUN_TEST(test_pin_protection_and_atomic_preflight);
    RUN_TEST(test_analog_digital_exclusion);
    RUN_TEST(test_pwm_six_channels_and_shared_timers);
    RUN_TEST(test_pwm_four_frequencies);
    RUN_TEST(test_controller_roles_and_profiles);
    RUN_TEST(test_parent_dependency_and_cascade);
    RUN_TEST(test_stop_stale_open_and_failed_cleanup);
    RUN_TEST(test_stop_has_reserved_job);
    RUN_TEST(test_base64_canonical_and_atomic_write);
    RUN_TEST(test_job_payload_ownership);
    RUN_TEST(test_capabilities_frames_and_revision);
    RUN_TEST(test_flow_rejects_busy_cycles);
    RUN_TEST(test_flow_preflight_is_atomic);
    RUN_TEST(test_flow_owns_resources_and_continues_offline);
    RUN_TEST(test_flow_stop_during_wait_prevents_future_calls);
    RUN_TEST(test_cancelled_open_cannot_rearm_after_flow_stop);
    RUN_TEST(test_parent_cleanup_fault_completes_stop_as_failed);
    RUN_TEST(test_bounded_result_history_is_explicitly_expired);
    RUN_TEST(test_admission_failure_preserves_all_pin_claims);
    RUN_TEST(test_planned_sleep_restores_next_step_and_variables);
    RUN_TEST(test_notification_subscription_is_session_scoped);
    RUN_TEST(test_retained_mqtt_and_duplicate_writes_never_execute);
    RUN_TEST(test_every_json_allocation_failure_releases_owned_data);
    RUN_TEST(test_discovery_survives_every_json_allocation_failure);
    RUN_TEST(test_rpc_ids_positional_and_standard_errors);
    RUN_TEST(test_rpc_nonce_and_sequence_are_independent_of_id);
    RUN_TEST(test_rpc_mixed_batch_and_limits);
    RUN_TEST(test_rpc_batch_stop_barrier_at_every_position);
    RUN_TEST(test_rpc_new_stop_interrupts_batch_between_items);
    RUN_TEST(test_rpc_batch_response_budget_and_notice);
    RUN_TEST(test_rpc_growing_output_never_redispatches_the_operation);
    RUN_TEST(test_rpc_notifications_still_execute_stop_and_guard_sequences);
    RUN_TEST(test_rpc_batch_and_notice_allocation_failures);
    return UNITY_END();
}
