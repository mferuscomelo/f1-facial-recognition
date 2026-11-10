idf_component_register(SRCS "board.cpp" "modem_power.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_psram spi_flash)
