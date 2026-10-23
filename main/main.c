#include "limivia.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "services.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

void app_main(void) {
    v3_json_init();
#ifdef LV_TEST_BOOT_FAILURE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
        esp_restart();
#endif
    v3_nvs_error = nvs_flash_init();
    v3_services_init();
    control_init();
    transport_init();
    v3_network_init();
    atomic_store(&v3_runtime_ready, true);
}
