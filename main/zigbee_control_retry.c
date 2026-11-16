#include "zigbee_control_retry.h"

static const uint32_t s_retry_offsets_ms[ZIGBEE_CONTROL_RETRY_ATTEMPTS] = {
    0U,
    6000U,
    12000U,
    24000U,
    48000U,
};

uint32_t zigbee_control_retry_offset_ms(uint8_t attempt)
{
    if (attempt >= ZIGBEE_CONTROL_RETRY_ATTEMPTS) {
        return s_retry_offsets_ms[ZIGBEE_CONTROL_RETRY_ATTEMPTS - 1U];
    }
    return s_retry_offsets_ms[attempt];
}

bool zigbee_control_retry_has_next(uint8_t attempt)
{
    return attempt < (ZIGBEE_CONTROL_RETRY_ATTEMPTS - 1U);
}
