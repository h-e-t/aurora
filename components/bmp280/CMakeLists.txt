file(GLOB SRCS "src/*.cpp")

idf_component_register(
    SRCS "${SRCS}"
    INCLUDE_DIRS "include"
    REQUIRES sensor esp_driver_i2c
)
