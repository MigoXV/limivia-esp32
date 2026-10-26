#include "ble_wire.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
// Independent, hard-bounded arenas permit RX/TX overlap without blocking NimBLE.
// 512-byte DEFLATE window and memLevel=1 fit into 16 KiB; allocation failure is explicit.
typedef struct { size_t used; unsigned char bytes[LV_WIRE_ARENA]; } arena;
static void *allocate(void *opaque, unsigned items, unsigned size) {
    arena *a = opaque;
    size_t n = (size_t)items * size;
    if (items && n / items != size) return NULL;
    n = (n + 7) & ~(size_t)7;
    if (n > sizeof(a->bytes) - a->used) return NULL;
    void *p = a->bytes + a->used;
    a->used += n;
    memset(p, 0, n);
    return p;
}
static void release(void *opaque, void *address) { (void)opaque; (void)address; }
static unsigned u16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }
static void put16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
size_t lv_wire_size(const uint8_t *p, size_t maximum) {
    if (p[0] != 'L' || p[1] != 'V' || p[2] != 4 || p[3] > 1) return 0;
    unsigned packed = u16(p + 4), raw = u16(p + 6);
    if (!packed || !raw || packed > maximum || raw > maximum ||
        (!p[3] && packed != raw)) return 0;
    return LV_WIRE_HEADER + packed;
}
size_t lv_wire_pack(const uint8_t *raw, size_t n, uint8_t *out, size_t capacity) {
    if (!n || n > 8192 || capacity < LV_WIRE_HEADER + n) return 0;
    size_t packed = n;
    bool compressed = false;
    arena *a = n >= 256 ? calloc(1, sizeof(arena)) : NULL;
    if (a) {
        z_stream z = {.zalloc = allocate, .zfree = release, .opaque = a};
        if (deflateInit2(&z, 1, Z_DEFLATED, 9, 1, Z_DEFAULT_STRATEGY) == Z_OK) {
            z.next_in = (Bytef *)raw; z.avail_in = n;
            z.next_out = out + LV_WIRE_HEADER; z.avail_out = n;
            if (deflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out * 100 <= n * 85) {
                packed = z.total_out; compressed = true;
            }
            deflateEnd(&z);
        }
        free(a);
    }
    if (!compressed) memcpy(out + LV_WIRE_HEADER, raw, n);
    out[0] = 'L'; out[1] = 'V'; out[2] = 4; out[3] = compressed;
    put16(out + 4, packed); put16(out + 6, n);
    uint32_t crc = crc32(0, raw, n);
    put16(out + 8, crc); put16(out + 10, crc >> 16);
    return LV_WIRE_HEADER + packed;
}
bool lv_wire_unpack(const uint8_t *wire, size_t n, uint8_t *out, size_t capacity, size_t *used) {
    if (n < LV_WIRE_HEADER || lv_wire_size(wire, capacity) != n) return false;
    size_t raw = u16(wire + 6), packed = n - LV_WIRE_HEADER;
    if (wire[3]) {
        arena *a = calloc(1, sizeof(arena));
        if (!a) return false;
        z_stream z = {.zalloc = allocate, .zfree = release, .opaque = a};
        bool ok = false;
        if (inflateInit2(&z, 9) == Z_OK) {
            z.next_in = (Bytef *)wire + LV_WIRE_HEADER; z.avail_in = packed;
            z.next_out = out; z.avail_out = raw;
            ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_in == packed && z.total_out == raw;
            inflateEnd(&z);
        }
        free(a);
        if (!ok) return false;
    } else memcpy(out, wire + LV_WIRE_HEADER, raw);
    if (crc32(0, out, raw) != u32(wire + 8)) return false;
    *used = raw;
    return true;
}
