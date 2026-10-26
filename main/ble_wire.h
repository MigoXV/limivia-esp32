#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LV_WIRE_HEADER 12
#define LV_WIRE_ARENA 16384
// 0 means malformed; no allocation occurs before header bounds validation.
size_t lv_wire_size(const uint8_t *header, size_t maximum);
size_t lv_wire_pack(const uint8_t *raw, size_t n, uint8_t *out, size_t capacity);
bool lv_wire_unpack(const uint8_t *wire, size_t n, uint8_t *out, size_t capacity, size_t *used);
