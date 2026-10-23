#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    void *context;
    int64_t (*now)(void *);
    bool (*current)(void *, uint32_t);
    // 1 accepted, 0 retryable, -1 terminal; callback itself must be bounded.
    int (*fragment)(void *, uint32_t, const char *, size_t, int64_t deadline);
    void (*pause)(void *);
    void (*abort)(void *, uint32_t);
} lv_tx_io;
bool lv_tx_frame(lv_tx_io, uint32_t generation, size_t payload, const char *, size_t);
