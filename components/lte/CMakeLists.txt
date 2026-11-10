idf_component_register(SRCS "lte.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_event esp_http_client
                       PRIV_REQUIRES board esp_driver_uart)
