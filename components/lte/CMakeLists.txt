idf_component_register(SRCS "lte.cpp" "modem_power.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_event esp_http_client
                       PRIV_REQUIRES esp_driver_i2c esp_driver_uart)
