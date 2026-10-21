#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define PROTOCOL_VERSION 1
#define REQUEST_MAX 1024
#define STATUS_LED 12
#define DEMO_LED 13

typedef enum { SOURCE_USB = 0, SOURCE_BLE = 1 } source_t;
typedef struct {
    source_t source;
    uint32_t generation;
    char text[REQUEST_MAX + 1];
} request_t;
void transport_init(void);
void transport_send(source_t source, uint32_t generation, const cJSON *message);
void transport_broadcast(const cJSON *message);
bool transport_ble_connected(void);
bool transport_usb_connected(void);
void control_init(void);
void control_submit(const request_t *request);
