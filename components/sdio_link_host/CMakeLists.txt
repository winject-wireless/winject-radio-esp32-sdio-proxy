idf_component_register(SRCS "sdio_link_host.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sdio_msg
                       PRIV_REQUIRES esp_driver_sdmmc sdmmc esp_driver_gpio freertos log heap)
