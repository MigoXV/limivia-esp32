#include "pwm_latch.h"
#include "unity.h"

typedef struct { int64_t now, latch_at, cancel_at; unsigned ticks; } fake;
static uint32_t read_duty(void *ctx) { fake *f = ctx; return f->now >= f->latch_at ? 42 : 9; }
static int64_t now_us(void *ctx) { return ((fake *)ctx)->now; }
static bool cancelled(void *ctx) { fake *f = ctx; return f->now >= f->cancel_at; }
static void tick(void *ctx) { fake *f = ctx; f->now += 1000; f->ticks++; }
static v3_pwm_latch_result run(fake *f, uint32_t hz) {
    v3_pwm_latch_io io = {.ctx=f, .read=read_duty, .now_us=now_us, .cancelled=cancelled, .wait_tick=tick};
    return v3_pwm_latch(&io, hz, 42);
}
void setUp(void) {}
void tearDown(void) {}
static void test_latched_immediately(void) {
    fake f = {.latch_at=0, .cancel_at=INT64_MAX};
    TEST_ASSERT_EQUAL(V3_PWM_READY, run(&f, 1000));
    TEST_ASSERT_EQUAL(0, f.ticks);
}
static void test_waits_for_actual_register(void) {
    fake f = {.latch_at=2000, .cancel_at=INT64_MAX};
    TEST_ASSERT_EQUAL(V3_PWM_READY, run(&f, 1000));
    TEST_ASSERT_EQUAL(2, f.ticks);
}
static void test_timeout_bounded(void) {
    fake f = {.latch_at=INT64_MAX, .cancel_at=INT64_MAX};
    TEST_ASSERT_EQUAL(V3_PWM_TIMEOUT, run(&f, 1000));
    TEST_ASSERT_EQUAL(22000, f.now);
}
static void test_cancel_before_and_during_wait(void) {
    fake f = {.latch_at=INT64_MAX, .cancel_at=0};
    TEST_ASSERT_EQUAL(V3_PWM_CANCELLED, run(&f, 1000));
    TEST_ASSERT_EQUAL(0, f.ticks);
    f.cancel_at=3000;
    TEST_ASSERT_EQUAL(V3_PWM_CANCELLED, run(&f, 1));
    TEST_ASSERT_EQUAL(3000, f.now);
}
static void test_low_frequency_budget_and_invalid_timer(void) {
    fake f = {.latch_at=1500000, .cancel_at=INT64_MAX};
    TEST_ASSERT_EQUAL(V3_PWM_READY, run(&f, 1));
    f.now=0; f.latch_at=INT64_MAX;
    TEST_ASSERT_EQUAL(V3_PWM_TIMEOUT, run(&f, 1));
    TEST_ASSERT_EQUAL(2020000, f.now);
    f.ticks=0;
    TEST_ASSERT_EQUAL(V3_PWM_BAD_FREQ, run(&f, 0));
    TEST_ASSERT_EQUAL(0, f.ticks);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_latched_immediately);
    RUN_TEST(test_waits_for_actual_register);
    RUN_TEST(test_timeout_bounded);
    RUN_TEST(test_cancel_before_and_during_wait);
    RUN_TEST(test_low_frequency_budget_and_invalid_timer);
    return UNITY_END();
}
