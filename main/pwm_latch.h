#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx);
    int64_t (*now_us)(void *ctx);
    bool (*cancelled)(void *ctx);
    void (*wait_tick)(void *ctx);
} v3_pwm_latch_io;
typedef enum { V3_PWM_READY, V3_PWM_TIMEOUT, V3_PWM_CANCELLED, V3_PWM_BAD_FREQ } v3_pwm_latch_result;

// Two actual timer periods plus scheduling margin; never block the actor.
static inline v3_pwm_latch_result v3_pwm_latch(const v3_pwm_latch_io *io,
                                             uint32_t frequency, uint32_t target) {
    if (!frequency)
        return V3_PWM_BAD_FREQ;
    int64_t deadline = io->now_us(io->ctx) + (2000000LL + frequency - 1) / frequency + 20000;
    for (;;) {
        if (io->cancelled(io->ctx))
            return V3_PWM_CANCELLED;
        if (io->read(io->ctx) == target)
            return V3_PWM_READY;
        if (io->now_us(io->ctx) >= deadline)
            return V3_PWM_TIMEOUT;
        io->wait_tick(io->ctx);
    }
}
