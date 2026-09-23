/*
 * Host-test stub for ESP-IDF's generated sdkconfig.h.
 *
 * The native test environment compiles the panel-independent display sources
 * (gfx.c, screens.c) so the layout can be exercised without hardware. Those
 * files pull in the CONFIG_S2_DISPLAY_* values; on the host there is no
 * generated sdkconfig, so the firmware defaults from src/Kconfig.projbuild are
 * mirrored here. Keep them in sync with the Kconfig defaults.
 */
#pragma once

#define CONFIG_S2_DISPLAY_ENABLE 1
#define CONFIG_S2_DISPLAY_POWER_FULL_SCALE_KW 70
#define CONFIG_S2_DISPLAY_REGEN_FULL_SCALE_KW 20
#define CONFIG_S2_TORQUE_COUNTS_PER_NM_X100 335
#define CONFIG_S2_DISPLAY_SCREENS 5
#define CONFIG_S2_DISPLAY_PRESSURE_PSI 1
#define CONFIG_S2_ACCEL_COUNTS_PER_G 7760

/* Screens cycle on a handlebar button; the info/scroll one by default. */
#define CONFIG_S2_DISPLAY_BUTTON_VEHICLE 1
#define CONFIG_S2_VEHICLE_BUTTON_INFO 1
