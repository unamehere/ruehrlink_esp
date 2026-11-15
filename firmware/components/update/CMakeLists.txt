idf_component_register(SRCS "update.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update esp_app_format esp_timer mbedtls)
