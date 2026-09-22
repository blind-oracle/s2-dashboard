/*
 * vehicle_button.h - use one of the motorcycle's own handlebar buttons as an
 * input, instead of a switch wired to a GPIO pin.
 *
 * The secondary bus broadcasts several momentary controls, so the screens can be
 * cycled from the bars with no extra wiring. The dash info/scroll button is the
 * natural one: it is the button the rider already uses to page through the
 * instrument cluster, and pressing it has no other effect on the bike.
 *
 * Pure C with no ESP-IDF runtime dependency beyond the generated sdkconfig.h, so
 * the button table and the edge detector are covered by the host unit tests.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "s2_dbc_gen.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One momentary control, as it appears in the broadcast database. */
typedef struct {
    uint16_t signal;        /* S2_SIG_* global signal index */
    uint64_t mask;          /* bits of the raw value that carry this control */
    uint64_t pressed;       /* value of those bits while it is held */
    const char *label;      /* for the boot banner and the log */
    const char *note;       /* database confidence and anything worth knowing */
} vbtn_def_t;

typedef enum {
    VBTN_INFO_SCROLL = 0,
    VBTN_HORN,
    VBTN_HIGHBEAM,
    VBTN_FRONT_BRAKE,
    VBTN_REAR_BRAKE,
    VBTN_HAZARDS,
    VBTN_CRUISE,
    VBTN__COUNT,
} vbtn_id_t;

/* Definition for one id, or NULL when the id is out of range. */
const vbtn_def_t *vbtn_get(vbtn_id_t id);

/* The button chosen in menuconfig, or NULL when vehicle buttons are not in use. */
const vbtn_def_t *vbtn_selected(void);

/* Edge-detector state. Zero-initialise; one instance per consumer. */
typedef struct {
    bool armed;             /* a first sample has been seen */
    bool prev_pressed;
    uint32_t prev_changes;
} vbtn_state_t;

/*
 * Feed one sample of the selected signal and get the number of presses since the
 * previous sample.
 *
 * `changes` is vs_signal_t::changes, which counts every transition of the raw
 * value. Using it as well as the current level means a press that began and
 * ended between two polls is still counted, rather than silently lost.
 *
 * The first sample only arms the detector, so a control already held when the
 * firmware starts does not register as a press. An invalid signal disarms it, so
 * a frame that stops and resumes does not fire a spurious press either.
 */
unsigned vbtn_feed(vbtn_state_t *st, const vbtn_def_t *def, bool valid, uint64_t raw,
                   uint32_t changes);

#ifdef __cplusplus
}
#endif
