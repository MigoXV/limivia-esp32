idf_component_register(SRCS "main.c" "transport.c" "control.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio esp_driver_ledc esp_driver_usb_serial_jtag esp_adc esp_timer esp_hw_support esp_app_format)
