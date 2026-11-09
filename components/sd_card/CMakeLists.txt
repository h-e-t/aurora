idf_component_register(
    SRCS "src/sd_card.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_spi esp_driver_sdspi esp_driver_gpio
             freertos log esp_timer
)
