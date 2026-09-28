# Sticky uses GPIO buttons only.
idf_component_register(
    SRCS
        "button_gpio.c"
        "iot_button.c"
    INCLUDE_DIRS
        "include"
        "interface"
    REQUIRES
        esp_driver_gpio
    PRIV_REQUIRES
        esp_timer
)

# Reads version: 4.1.6 from idf_component.yml and exports the original
# BUTTON_VER_MAJOR / BUTTON_VER_MINOR / BUTTON_VER_PATCH compile definitions
# used by iot_button.c. This does not add ADC or matrix implementations.
include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
