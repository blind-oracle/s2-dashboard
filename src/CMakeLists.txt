# The project's 'src' directory is registered by PlatformIO as an extra ESP-IDF
# component named 'src'. Unlike a 'main' component it does not automatically
# depend on every other component, so requirements are listed explicitly.

file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_LIST_DIR}/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "gen" "${CMAKE_CURRENT_LIST_DIR}/../include"
    REQUIRES esp_driver_twai esp_timer esp_ringbuf app_update
    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
