idf_component_register(SRCS "kenwood.c" "setup_cert.c"
                       INCLUDE_DIRS "include"
                       REQUIRES appliance
                       PRIV_REQUIRES esp_https_server esp_http_server esp_http_client esp_timer net mbedtls led
                       EMBED_TXTFILES "certs/machine_cert.pem" "certs/machine_key.pem" "certs/setup_server.pem")
