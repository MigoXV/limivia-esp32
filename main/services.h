#pragma once
#include "v3.h"
#include "esp_err.h"
#include <stdatomic.h>
extern esp_err_t v3_nvs_error;
extern atomic_bool v3_inhibit_requested;
extern atomic_bool v3_inhibit_value, v3_runtime_ready, v3_boot_healthy;
extern atomic_bool v3_inhibit_busy;
extern atomic_int v3_inhibit_error;
bool v3_autostart_inhibited(void);
void v3_services_init(void);
int v3_system_work(const v3_work *, cJSON **, bool *);
cJSON *v3_system_status(void);
void v3_services_poll(void);
bool v3_storage_ready(void);
int v3_storage_load(const char *, cJSON **);
int v3_storage_save(const char *, const char *, size_t);
int v3_storage_read_bytes(const char *, bool secret, char **, size_t *);
bool v3_storage_name(const char *);
int v3_network_work(const v3_work *, cJSON **, bool *);
void v3_network_init(void);
void v3_network_poll(void);
bool v3_network_maintenance(void);
bool v3_flash_operation(const char *, const cJSON *);
void v3_inhibit_latch(bool);
cJSON *v3_checkpoint_take(void);
int v3_network_modem_sleep(const char *);
