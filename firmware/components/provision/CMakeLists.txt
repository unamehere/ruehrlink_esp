idf_component_register(SRCS "provision.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash esp_timer esp_wifi esp_app_format protocomm net api led)
