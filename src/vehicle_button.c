#include "vehicle_button.h"

/*
 * Raw values are from can-db/livewire_s2_delmar_secondary.dbc. Each entry needs a
 * mask as well as a value because not every control owns its whole byte: the
 * cruise arm state is one bit of a byte whose other bits vary independently.
 */
static const vbtn_def_t s_buttons[VBTN__COUNT] = {
    [VBTN_INFO_SCROLL] = {
        .signal = S2_SIG_LEFT_SWITCH_354_info_scroll_button,
        .mask = 0xFF,
        .pressed = 0x01,
        .label = "info/scroll",
        .note = "0x354 D1, STRONG - the dash menu button, no side effects",
    },
    [VBTN_HORN] = {
        .signal = S2_SIG_LEFT_SWITCH_354_horn,
        .mask = 0xFF,
        .pressed = 0x01,
        .label = "horn",
        .note = "0x354 D2, CONFIRMED - sounds the horn as well",
    },
    [VBTN_HIGHBEAM] = {
        .signal = S2_SIG_LIGHTING_152_highbeam,
        .mask = 0x04,
        .pressed = 0x04,
        .label = "high beam",
        .note = "0x152 D3, STRONG - a flash-to-pass tap also cycles",
    },
    [VBTN_FRONT_BRAKE] = {
        .signal = S2_SIG_FRONT_BRAKE_HAZARD_15A_front_brake,
        .mask = 0x40,
        .pressed = 0x40,
        .label = "front brake",
        .note = "0x15A D6, CONFIRMED - fires on every brake application",
    },
    [VBTN_REAR_BRAKE] = {
        .signal = S2_SIG_IGNITION_REAR_BRAKE_133_rear_brake,
        .mask = 0x10,
        .pressed = 0x10,
        .label = "rear brake",
        .note = "0x133 D4, CONFIRMED - fires on every brake application",
    },
    [VBTN_HAZARDS] = {
        .signal = S2_SIG_FRONT_BRAKE_HAZARD_15A_hazards,
        .mask = 0x10,
        .pressed = 0x10,
        .label = "hazards",
        .note = "0x15A D2, CONFIRMED byte - may follow the blink, not the switch",
    },
    [VBTN_CRUISE] = {
        .signal = S2_SIG_SPEED_160_cruise_armed,
        .mask = 0x80,
        .pressed = 0x80,
        .label = "cruise arm",
        .note = "0x160 D6 bit 7, CONFIRMED - latching, one press per arm/disarm",
    },
};

const vbtn_def_t *vbtn_get(vbtn_id_t id)
{
    if ((unsigned)id >= (unsigned)VBTN__COUNT) {
        return NULL;
    }
    return &s_buttons[id];
}

const vbtn_def_t *vbtn_selected(void)
{
#if CONFIG_S2_DISPLAY_BUTTON_VEHICLE
/* One branch per member of the Kconfig choice, so a renamed symbol shows up. */
#if CONFIG_S2_VEHICLE_BUTTON_INFO
    return vbtn_get(VBTN_INFO_SCROLL);
#elif CONFIG_S2_VEHICLE_BUTTON_HORN
    return vbtn_get(VBTN_HORN);
#elif CONFIG_S2_VEHICLE_BUTTON_HIGHBEAM
    return vbtn_get(VBTN_HIGHBEAM);
#elif CONFIG_S2_VEHICLE_BUTTON_FRONT_BRAKE
    return vbtn_get(VBTN_FRONT_BRAKE);
#elif CONFIG_S2_VEHICLE_BUTTON_REAR_BRAKE
    return vbtn_get(VBTN_REAR_BRAKE);
#elif CONFIG_S2_VEHICLE_BUTTON_HAZARDS
    return vbtn_get(VBTN_HAZARDS);
#elif CONFIG_S2_VEHICLE_BUTTON_CRUISE
    return vbtn_get(VBTN_CRUISE);
#else
    return vbtn_get(VBTN_INFO_SCROLL);   /* defensive: the choice default */
#endif
#else
    return NULL;
#endif
}

vbtn_event_t vbtn_feed(vbtn_state_t *st, const vbtn_def_t *def, bool valid, uint64_t raw,
                       uint32_t changes, int64_t now_us, int64_t long_press_us)
{
    vbtn_event_t ev = { 0, false };

    if (!st || !def) {
        return ev;
    }
    if (!valid) {
        st->armed = false;
        st->long_fired = false;
        return ev;
    }

    bool pressed = (raw & def->mask) == def->pressed;

    if (!st->armed) {
        st->armed = true;
        st->prev_pressed = pressed;
        st->prev_changes = changes;
        st->press_since_us = now_us;
        /*
         * A control already held when we start reports nothing: marking the hold
         * as having already fired suppresses the long press now and the short
         * press when it is eventually released.
         */
        st->long_fired = pressed;
        return ev;
    }

    /*
     * Unsigned subtraction, so a wrap of the 32-bit change counter still gives
     * the right delta. The control alternates between released and held, so the
     * number of completed presses - that is, transitions back into released -
     * follows from the delta and the two endpoint levels.
     */
    uint32_t delta = changes - st->prev_changes;
    unsigned completed;
    if (!st->prev_pressed && !pressed) {
        completed = (unsigned)(delta / 2u);             /* even count, still released */
    } else if (!st->prev_pressed && pressed) {
        /* Odd count, ends held: the final press has not been released yet. */
        completed = delta ? (unsigned)((delta - 1u) / 2u) : 0u;
    } else if (st->prev_pressed && !pressed) {
        /*
         * Odd count, ends released. The guard matters: a level that changed
         * without the counter advancing is inconsistent input, and delta + 1
         * would still be right but delta itself must not be trusted as 0.
         */
        completed = delta ? (unsigned)((delta + 1u) / 2u) : 1u;
    } else {
        completed = (unsigned)(delta / 2u);             /* even count, still held */
    }

    /*
     * A hold that already reported a long press must not also report a short
     * press when it ends, so drop the completion that closes it.
     */
    if (st->long_fired && completed > 0) {
        completed--;
        st->long_fired = false;
    }

    /*
     * Restart the hold timer whenever a new hold begins: either the level went
     * released -> held, or it was held at both samples but the counter moved,
     * which means it was released and pressed again in between.
     */
    if (pressed && (!st->prev_pressed || delta >= 2)) {
        st->press_since_us = now_us;
        st->long_fired = false;
    }
    if (!pressed) {
        st->long_fired = false;
    }

    if (pressed && !st->long_fired && long_press_us > 0 &&
        (now_us - st->press_since_us) >= long_press_us) {
        ev.long_press = true;
        st->long_fired = true;
    }

    ev.presses = completed;
    st->prev_pressed = pressed;
    st->prev_changes = changes;
    return ev;
}
