#include "bh1750_protocol.h"

#include <math.h>

float bh1750_protocol_decode_lux(uint8_t msb, uint8_t lsb)
{
    uint16_t raw = ((uint16_t)msb << 8) | lsb;
    return (float)raw / 1.2f;
}

bool bh1750_protocol_lux_valid(float lux)
{
    return isfinite(lux) && lux >= 0.0f && lux <= 54612.5f;
}

float bh1750_protocol_median(const float *values, size_t count)
{
    if (!values || count == 0 || count > 3) return NAN;
    float sorted[3] = {0};
    for (size_t i = 0; i < count; ++i) {
        sorted[i] = values[i];
        for (size_t j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) {
            float temporary = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = temporary;
        }
    }
    return sorted[count / 2];
}
