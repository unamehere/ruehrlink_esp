idf_component_register(SRCS "storage.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES fatfs wear_levelling vfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio esp_partition)
