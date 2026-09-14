/*
 * uds_client.h - ISO 15765-2 (ISO-TP) transport + UDS 0x22 ReadDataByIdentifier
 * poller over the catalogued diagnostic modules. Strictly one request in flight,
 * so multi-frame responses from a shared responder id can never interleave.
 *
 * Compiled to stubs unless CONFIG_S2_UDS_ENABLE (which requires normal, not
 * listen-only, CAN mode because it transmits on the motorcycle's bus).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can_bus.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Padding byte for request frames shorter than 8 bytes. */
#define UDS_PADDING_BYTE 0x00

#define UDS_MAX_PAYLOAD 512   /* largest catalogued DID is 400 B (EHCU 021B) */

typedef struct {
    uint32_t requests;
    uint32_t positive;
    uint32_t negative;
    uint32_t timeouts;
    uint32_t tx_errors;
    uint32_t transport_errors;   /* bad sequence numbers, oversize, etc. */
    uint32_t unattributed_nrc;   /* NRCs seen while another tester used the same request id */
    uint32_t backoffs;           /* times the poller waited for another tester to go quiet */
    uint32_t rounds;
} uds_stats_t;

esp_err_t uds_client_start(void);

/* Called from the decoder task for every frame with id 0x7D0..0x7FF. Never blocks. */
void uds_client_on_frame(const can_frame_t *f);

void uds_client_get_stats(uds_stats_t *out);

/*
 * Blocking single read (used by the poller; exposed for future console commands,
 * serialised by an internal mutex). Returns ESP_OK with *len bytes of DID data in
 * buf, ESP_ERR_INVALID_RESPONSE with *nrc set on a negative response,
 * ESP_ERR_INVALID_SIZE on an ISO-TP transport error, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t uds_read_did(uint16_t req_id, uint16_t resp_id, uint16_t did, uint8_t *buf, size_t *len, uint8_t *nrc);

const char *uds_nrc_name(uint8_t nrc);

#ifdef __cplusplus
}
#endif
