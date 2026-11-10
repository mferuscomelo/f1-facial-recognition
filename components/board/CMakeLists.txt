idf_component_register(SRCS "board.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_psram spi_flash
                       PRIV_REQUIRES)
