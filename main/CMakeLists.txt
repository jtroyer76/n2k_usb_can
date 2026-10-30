idf_component_register(
    SRCS
        "main.c"
        "can_bus.c"
        "gs_usb_dev.c"
        "status_led.c"
        "usb_console.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_twai esp_driver_gpio esp_timer soc
)
