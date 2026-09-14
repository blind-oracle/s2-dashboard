#include "dbc_decode.h"

#include <math.h>
#include <stdio.h>

bool s2_signal_fits(const s2_signal_def_t *sig, size_t dlc)
{
    if (sig->length == 0 || sig->length > 64 || sig->start_bit > 63) {
        return false;
    }
    size_t total_bits = dlc * 8u;
    if (sig->byte_order == S2_ORDER_BIG_ENDIAN) {
        /* Position of the MSB in "big-endian linear" bit numbering (bit 0 = MSB of byte 0). */
        size_t msb = (sig->start_bit / 8u) * 8u + (7u - (sig->start_bit % 8u));
        return msb + sig->length <= total_bits;
    }
    return (size_t)sig->start_bit + sig->length <= total_bits;
}

uint64_t s2_extract_raw(const uint8_t *data, const s2_signal_def_t *sig)
{
    uint64_t value = 0;
    if (sig->byte_order == S2_ORDER_BIG_ENDIAN) {
        unsigned pos = (sig->start_bit / 8u) * 8u + (7u - (sig->start_bit % 8u));
        for (unsigned i = 0; i < sig->length; i++, pos++) {
            unsigned byte = pos / 8u;
            unsigned bit = 7u - (pos % 8u);
            value = (value << 1) | ((data[byte] >> bit) & 1u);
        }
    } else {
        for (unsigned i = 0; i < sig->length; i++) {
            unsigned pos = sig->start_bit + i;
            unsigned byte = pos / 8u;
            unsigned bit = pos % 8u;
            value |= (uint64_t)((data[byte] >> bit) & 1u) << i;
        }
    }
    return value;
}

int64_t s2_raw_to_int(const s2_signal_def_t *sig, uint64_t raw)
{
    if (!sig->is_signed || sig->length >= 64) {
        return (int64_t)raw;
    }
    uint64_t sign_bit = 1ull << (sig->length - 1u);
    if (raw & sign_bit) {
        return (int64_t)(raw | ~((sign_bit << 1) - 1ull));
    }
    return (int64_t)raw;
}

double s2_raw_to_physical(const s2_signal_def_t *sig, uint64_t raw)
{
    return (double)s2_raw_to_int(sig, raw) * sig->factor + sig->offset;
}

int s2_signal_decimals(const s2_signal_def_t *sig)
{
    /* Smallest number of decimals (0..4) that represents the resolution exactly. */
    double f = fabs(sig->factor);
    double o = fabs(sig->offset);
    for (int d = 0; d <= 4; d++) {
        double scale = pow(10.0, d);
        double fs = f * scale;
        double os = o * scale;
        if (fabs(fs - floor(fs + 0.5)) < 1e-6 && fabs(os - floor(os + 0.5)) < 1e-6) {
            return d;
        }
    }
    return 4;
}

int s2_format_value(const s2_signal_def_t *sig, double value, char *buf, size_t buflen)
{
    int d = s2_signal_decimals(sig);
    if (d == 0) {
        return snprintf(buf, buflen, "%lld", (long long)llround(value));
    }
    return snprintf(buf, buflen, "%.*f", d, value);
}
