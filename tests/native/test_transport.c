#include "v3.h"
#include "tx.h"
#include "unity.h"
#include <string.h>
void setUp(void) {}
void tearDown(void) {}
static int frames;
static size_t frame_len;
static char last_frame[1025];
static void got_frame(void *ctx, const char *text, size_t n) {
    (void)ctx;
    ++frames;
    frame_len = n;
    memcpy(last_frame, text, n);
    last_frame[n] = 0;
}
static void test_framing_boundaries_and_recovery(void) {
    lv_rx rx = {0};
    frames = 0;
    char data[1027];
    memset(data, ' ', sizeof(data));
    data[1024] = '\n';
    lv_receive(&rx, (const uint8_t *)data, 512, 100, got_frame, NULL);
    lv_receive(&rx, (const uint8_t *)data + 512, 513, 101, got_frame, NULL);
    TEST_ASSERT_EQUAL(1, frames);
    TEST_ASSERT_EQUAL(1024, frame_len);
    data[1024] = ' ';
    data[1025] = '\n';
    lv_receive(&rx, (const uint8_t *)data, 1026, 102, got_frame, NULL);
    TEST_ASSERT_EQUAL_STRING("!", last_frame);
    const uint8_t nul[] = {'a', 0, '\n'};
    lv_receive(&rx, nul, 3, 103, got_frame, NULL);
    TEST_ASSERT_EQUAL_STRING("!", last_frame);
    lv_receive(&rx, (const uint8_t *)"abc", 3, 104, got_frame, NULL);
    lv_receive(&rx, (const uint8_t *)"d\nnext\n", 7, 3105, got_frame, NULL);
    TEST_ASSERT_EQUAL_STRING("next", last_frame);
    TEST_ASSERT_EQUAL(5, frames);
}
static struct {
    int64_t now;
    int sends, abort, fail_after;
    uint32_t generation;
    size_t max_fragment;
} tx;
static int64_t tx_now(void *p) {
    (void)p;
    return tx.now;
}
static bool tx_current(void *p, uint32_t generation) {
    (void)p;
    return generation == tx.generation;
}
static int tx_fragment(void *p, uint32_t generation, const char *bytes, size_t n,
                       int64_t deadline) {
    (void)p;
    (void)generation;
    (void)bytes;
    (void)deadline;
    ++tx.sends;
    if (n > tx.max_fragment)
        tx.max_fragment = n;
    return tx.fail_after && tx.sends > tx.fail_after ? 0 : 1;
}
static void tx_pause(void *p) {
    (void)p;
    tx.now += 10;
}
static void tx_abort(void *p, uint32_t generation) {
    (void)p;
    (void)generation;
    ++tx.abort;
}
static void test_ble_frame_deadline_and_generation(void) {
    lv_tx_io io = {.now = tx_now,
                   .current = tx_current,
                   .fragment = tx_fragment,
                   .pause = tx_pause,
                   .abort = tx_abort};
    char line[1024] = {0};
    memset(&tx, 0, sizeof(tx));
    tx.generation = 1;
    TEST_ASSERT_TRUE(lv_tx_frame(io, 1, 20, line, sizeof(line)));
    TEST_ASSERT_EQUAL(20, tx.max_fragment);
    TEST_ASSERT_EQUAL(52, tx.sends);
    memset(&tx, 0, sizeof(tx));
    tx.generation = 1;
    tx.fail_after = 1;
    TEST_ASSERT_FALSE(lv_tx_frame(io, 1, 180, line, sizeof(line)));
    TEST_ASSERT_EQUAL(3000, tx.now);
    TEST_ASSERT_EQUAL(1, tx.abort);
    tx.generation = 2;
    int before = tx.sends;
    TEST_ASSERT_FALSE(lv_tx_frame(io, 1, 20, line, sizeof(line)));
    TEST_ASSERT_EQUAL(before, tx.sends);
    TEST_ASSERT_EQUAL(1, tx.abort);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_framing_boundaries_and_recovery);
    RUN_TEST(test_ble_frame_deadline_and_generation);
    return UNITY_END();
}
