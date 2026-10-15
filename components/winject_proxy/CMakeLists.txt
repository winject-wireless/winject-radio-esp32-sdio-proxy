idf_component_register(SRCS "winject_proxy.c" "ota_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sdio_link_host
                       PRIV_REQUIRES lwip freertos log esp_http_server app_update esp_system)
