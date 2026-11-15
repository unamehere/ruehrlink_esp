idf_component_register(SRCS "sequencer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event
                       PRIV_REQUIRES appliance esp_timer)
