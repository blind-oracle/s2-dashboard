#include "e2e.h"

uint8_t s2_crc8_j1850(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (uint8_t)((crc & 0x80u) ? ((crc << 1) ^ 0x1Du) : (crc << 1));
        }
    }
    return (uint8_t)(crc ^ 0xFFu);
}

bool s2_e2e_crc_ok(const uint8_t *data, size_t dlc)
{
    if (dlc != 8) {
        return false;
    }
    return s2_crc8_j1850(data, 7) == data[7];
}

uint32_t s2_e2e_track_alive(s2_e2e_tracker_t *t, uint8_t alive)
{
    alive &= S2_E2E_ALIVE_MASK;
    if (!t->have_last) {
        t->have_last = true;
        t->last = alive;
        return 0;
    }
    uint8_t delta = (uint8_t)((alive - t->last) & S2_E2E_ALIVE_MASK);
    t->last = alive;
    if (delta == 1) {
        return 0;
    }
    uint32_t lost = (delta == 0) ? (S2_E2E_ALIVE_MOD - 1) : (uint32_t)(delta - 1);
    t->gaps++;
    t->frames_lost += lost;
    return lost;
}
