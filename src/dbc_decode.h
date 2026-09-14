/*
 * dbc_decode.h - generic DBC signal extraction (Motorola / Intel) and scaling.
 * Pure C, shared with the host unit tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "s2_dbc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the signal lies entirely inside a payload of dlc bytes. */
bool s2_signal_fits(const s2_signal_def_t *sig, size_t dlc);

/*
 * Extract the raw bit field. Big-endian signals use the DBC (Motorola, "@0")
 * start-bit convention: start_bit is the position of the most significant bit,
 * numbered 7..0 within each byte, and the field continues into the following
 * bytes. Little-endian signals start at the least significant bit.
 * The caller must have checked s2_signal_fits().
 */
uint64_t s2_extract_raw(const uint8_t *data, const s2_signal_def_t *sig);

/* Interpret raw as signed (two's complement in `length` bits) when the DBC says so. */
int64_t s2_raw_to_int(const s2_signal_def_t *sig, uint64_t raw);

/* value = raw * factor + offset */
double s2_raw_to_physical(const s2_signal_def_t *sig, uint64_t raw);

/* Number of decimals that represent the signal's resolution (0..4). */
int s2_signal_decimals(const s2_signal_def_t *sig);

/* Print a physical value with a sensible number of decimals. Returns chars written. */
int s2_format_value(const s2_signal_def_t *sig, double value, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
