idf_component_register(SRCS "led.c" "strip.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc)
