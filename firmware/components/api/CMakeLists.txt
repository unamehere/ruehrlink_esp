idf_component_register(SRCS "api.c" "auth.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server esp_driver_gpio esp_timer esp_app_format nvs_flash
                                     appliance net storage led factory sequencer update)
