idf_component_register(SRCS "net.c" "dns.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash lwip esp_app_format)
