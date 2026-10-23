#pragma once
#include "v3.h"
void control_init(void);
void control_submit(lv_source, uint32_t generation, const char *, size_t);
void control_submit_message(lv_source, uint32_t, const char *, size_t, bool retained);
void control_wake(void);
void transport_init(void);
bool transport_send(lv_source, uint32_t generation, const char *, size_t);
uint32_t transport_generation(lv_source);
bool transport_ble_connected(void);
uint16_t transport_ble_mtu(void);
bool network_send(lv_source, uint32_t, const char *, size_t);
uint32_t network_generation(lv_source);
