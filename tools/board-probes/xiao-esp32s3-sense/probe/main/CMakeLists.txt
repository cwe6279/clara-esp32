idf_component_register(SRCS "probe.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_ledc esp_driver_i2s esp_driver_sdspi sdmmc esp_psram esp_hw_support spi_flash esp_timer)
