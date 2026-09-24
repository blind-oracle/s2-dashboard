#include "vehicle_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static vs_signal_t s_signals[S2_SIG__COUNT];
static vs_message_t s_messages[S2_DBC_MESSAGE_COUNT];
static vs_unknown_t s_unknown[VS_UNKNOWN_MAX];
static unsigned s_unknown_count;
static uint32_t s_unknown_overflow;
static vs_derived_t s_derived;
static vs_uds_t s_uds;
static ride_extremes_t s_extremes;
static SemaphoreHandle_t s_mutex;

void vs_init(void)
{
    memset(s_signals, 0, sizeof(s_signals));
    memset(s_messages, 0, sizeof(s_messages));
    memset(s_unknown, 0, sizeof(s_unknown));
    memset(&s_derived, 0, sizeof(s_derived));
    memset(&s_uds, 0, sizeof(s_uds));
    ride_extremes_reset(&s_extremes);
    s_unknown_count = 0;
    s_mutex = xSemaphoreCreateMutex();
}

void vs_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void vs_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

vs_signal_t *vs_signal(uint16_t index)
{
    return index < S2_SIG__COUNT ? &s_signals[index] : NULL;
}

vs_message_t *vs_message(uint16_t message_index)
{
    return message_index < S2_DBC_MESSAGE_COUNT ? &s_messages[message_index] : NULL;
}

vs_derived_t *vs_derived(void)
{
    return &s_derived;
}

vs_uds_t *vs_uds(void)
{
    return &s_uds;
}

ride_extremes_t *vs_extremes(void)
{
    return &s_extremes;
}

bool vs_update_signal(uint16_t index, uint64_t raw, double value, int64_t ts_us)
{
    vs_signal_t *s = vs_signal(index);
    if (!s) {
        return false;
    }
    bool changed = !s->valid || s->raw != raw;
    s->raw = raw;
    s->value = value;
    s->ts_us = ts_us;
    s->updates++;
    if (changed) {
        s->changes++;
        s->change_ts_us = ts_us;
    }
    s->valid = true;
    return changed;
}

vs_unknown_t *vs_note_unknown(uint32_t id, const uint8_t *data, uint8_t dlc, int64_t ts_us, bool *changed)
{
    vs_unknown_t *slot = NULL;
    for (unsigned i = 0; i < s_unknown_count; i++) {
        if (s_unknown[i].id == id) {
            slot = &s_unknown[i];
            break;
        }
    }
    bool is_new = false;
    if (!slot) {
        if (s_unknown_count >= VS_UNKNOWN_MAX) {
            s_unknown_overflow++;
            *changed = false;
            return NULL;
        }
        slot = &s_unknown[s_unknown_count++];
        memset(slot, 0, sizeof(*slot));
        slot->id = id;
        is_new = true;
    }
    bool diff = is_new || slot->dlc != dlc || memcmp(slot->data, data, dlc) != 0;
    slot->count++;
    if (diff) {
        slot->changes++;
    }
    slot->dlc = dlc;
    memcpy(slot->data, data, dlc);
    if (dlc < 8) {
        memset(slot->data + dlc, 0, 8 - dlc);
    }
    slot->last_ts_us = ts_us;
    *changed = diff;
    return slot;
}

const vs_unknown_t *vs_unknown_table(unsigned *count)
{
    *count = s_unknown_count;
    return s_unknown;
}

uint32_t vs_unknown_overflow(void)
{
    return s_unknown_overflow;
}
