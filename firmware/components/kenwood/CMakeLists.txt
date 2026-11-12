idf_component_register(SRCS "kenwood.c"
                       INCLUDE_DIRS "include"
                       REQUIRES appliance
                       PRIV_REQUIRES esp_https_server esp_http_server esp_timer
                       EMBED_TXTFILES "certs/machine_cert.pem" "certs/machine_key.pem")
