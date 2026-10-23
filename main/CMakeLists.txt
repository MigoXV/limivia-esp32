idf_component_register(SRCS "main.c" "transport.c" "runtime.c" "engine.c" "rpc.c" "flows.c" "operations.c" "json.c" "resources.c" "peripherals.c" "buses.c" "signals.c" "system.c" "network.c" "framing.c" "tx.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio esp_driver_ledc esp_driver_usb_serial_jtag esp_adc esp_timer esp_hw_support esp_app_format
    esp_driver_uart esp_driver_i2c esp_driver_spi esp_driver_twai esp_driver_rmt esp_driver_i2s esp_driver_sdm esp_driver_tsens
    esp_wifi esp_event esp_netif esp_http_server app_update esp_pm mbedtls joltwallet__littlefs espressif__mqtt)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_PROFILE="${LIMIVIA_PROFILE}")
option(LV_TEST_BOOT_FAILURE "OTA integration fixture: reset before confirming a pending image" OFF)
if(LV_TEST_BOOT_FAILURE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_TEST_BOOT_FAILURE=1)
endif()
if(LIMIVIA_PROFILE STREQUAL "signal")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_HAS_SIGNAL=1 LV_HAS_BUS=0 LV_HAS_SLAVE=0 LV_HAS_IOT=0)
elseif(LIMIVIA_PROFILE STREQUAL "iot")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_HAS_SIGNAL=0 LV_HAS_BUS=1 LV_HAS_SLAVE=0 LV_HAS_IOT=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_HAS_SIGNAL=0 LV_HAS_BUS=1 LV_HAS_SLAVE=1 LV_HAS_IOT=0)
endif()
