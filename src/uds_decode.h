/*
 * uds_decode.h - per-DID interpretation of UDS 0x22 responses, following the
 * formulas in can-db/uds_catalog.json. Unknown / candidate DIDs are hex-dumped.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "s2_uds_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

void uds_decode_log(const s2_uds_module_t *m, const s2_uds_did_t *d, const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif
