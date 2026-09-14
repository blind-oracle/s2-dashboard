/*
 * decoder.h - CAN frame dispatch: E2E validation, DBC signal extraction,
 * vehicle-state updates, change logging and diagnostic-frame forwarding.
 */
#pragma once

#include "can_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frames;
    uint32_t known;
    uint32_t unknown;
    uint32_t diag;          /* 0x7D0..0x7FF UDS traffic */
    uint32_t crc_fail;
    uint32_t alive_gaps;
    uint32_t frames_lost;   /* estimated from alive-counter gaps */
    uint32_t dlc_mismatch;
} decoder_stats_t;

void decoder_task(void *arg);
void decoder_handle_frame(const can_frame_t *f);
void decoder_get_stats(decoder_stats_t *out);

#ifdef __cplusplus
}
#endif
