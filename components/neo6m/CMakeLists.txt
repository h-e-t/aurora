# Find all source files inside the src directory
file(GLOB SRCS "src/*.cpp")


idf_component_register(
    SRCS "${SRCS}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES sensor esp_driver_uart
)
