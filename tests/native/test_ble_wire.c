#include "ble_wire.h"
#include "unity.h"
#include <string.h>
void setUp(void) {}
void tearDown(void) {}
static void test_roundtrip_bounds_and_crc(void) {
    uint8_t raw[8192], wire[8204], decoded[8193];
    memset(raw, 'a', sizeof(raw));
    for (size_t size = 1; size <= 8192; size *= 2) {
        size_t packed = lv_wire_pack(raw, size, wire, sizeof(wire));
        TEST_ASSERT_GREATER_THAN(12, packed);
        TEST_ASSERT_EQUAL(packed, lv_wire_size(wire, 8192));
        size_t used = 0;
        memset(decoded, 0x5a, sizeof(decoded));
        TEST_ASSERT_TRUE(lv_wire_unpack(wire, packed, decoded, size, &used));
        TEST_ASSERT_EQUAL(size, used);
        TEST_ASSERT_EQUAL_MEMORY(raw, decoded, size);
        TEST_ASSERT_EQUAL(0x5a, decoded[size]);
        if (size >= 256) TEST_ASSERT_EQUAL(1, wire[3]);
        wire[8] ^= 1;
        TEST_ASSERT_FALSE(lv_wire_unpack(wire, packed, decoded, sizeof(raw), &used));
    }
    TEST_ASSERT_EQUAL(0, lv_wire_pack(raw, 8193, wire, sizeof(wire)));
}
static void test_corrupt_header_and_decompression_bound(void) {
    uint8_t raw[1024], wire[1036], decoded[2] = {0x5a, 0x5a};
    memset(raw, 'b', sizeof(raw));
    size_t n = lv_wire_pack(raw, sizeof(raw), wire, sizeof(wire)), used;
    wire[6] = 1; wire[7] = 0;
    TEST_ASSERT_FALSE(lv_wire_unpack(wire, n, decoded, 1, &used));
    TEST_ASSERT_EQUAL(0x5a, decoded[1]);
    wire[2] = 3;
    TEST_ASSERT_EQUAL(0, lv_wire_size(wire, 1024));
    wire[2] = 4; wire[4] = 0; wire[5] = 0;
    TEST_ASSERT_EQUAL(0, lv_wire_size(wire, 1024));
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_bounds_and_crc);
    RUN_TEST(test_corrupt_header_and_decompression_bound);
    return UNITY_END();
}
