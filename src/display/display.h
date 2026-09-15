/*
 * display.h - the display task: samples the vehicle state, renders a screen and
 * pushes it to the panel. Also owns the screen-select button.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the panel and starts the render task. */
esp_err_t display_start(void);

/* Screen currently shown, 0-based. */
unsigned display_current_screen(void);

/* Advance to the next screen (what a short button press does). */
void display_next_screen(void);

#ifdef __cplusplus
}
#endif
