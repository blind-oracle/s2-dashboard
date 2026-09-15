# The project's 'src' directory is registered by PlatformIO as an extra ESP-IDF
# component named 'src'. Unlike a 'main' component it does not automatically
# depend on every other component, so requirements are listed explicitly.
#
# The sources are listed rather than globbed on purpose: PlatformIO snapshots the
# source list when CMake last ran and never re-runs ninja, so a globbed new file
# is silently left out of the build until this file is touched. Editing this file
# is itself a reconfigure trigger, so adding a source here cannot be forgotten.

idf_component_register(
    SRCS
        "main.c"
        "ble_proto.c"
        "ble_telemetry.c"
        "can_bus.c"
        "dbc_decode.c"
        "decoder.c"
        "e2e.c"
        "log_writer.c"
        "s2_overlay.c"
        "status_led.c"
        "summary.c"
        "uds_client.c"
        "uds_decode.c"
        "vehicle_state.c"
        "gen/s2_dbc_gen.c"
        "gen/s2_font5x7.c"
        "gen/s2_uds_gen.c"
        "display/display.c"
        "display/gfx.c"
        "display/panel.c"
        "display/screens.c"
    INCLUDE_DIRS "." "gen" "display" "${CMAKE_CURRENT_LIST_DIR}/../include"
    REQUIRES esp_driver_twai esp_timer esp_ringbuf app_update
    PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_driver_rmt esp_driver_spi esp_driver_ledc esp_lcd driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
