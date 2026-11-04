#include "sht31_protocol.h"

#include <math.h>

uint8_t sht31_protocol_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; data && i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool sht31_protocol_decode(const uint8_t frame[6], float *temperature_c,
                           float *humidity_percent)
{
    if (!frame || !temperature_c || !humidity_percent ||
        sht31_protocol_crc8(frame, 2) != frame[2] ||
        sht31_protocol_crc8(frame + 3, 2) != frame[5]) return false;
    uint16_t raw_temperature = ((uint16_t)frame[0] << 8) | frame[1];
    uint16_t raw_humidity = ((uint16_t)frame[3] << 8) | frame[4];
    float temperature = -45.0f + 175.0f * (float)raw_temperature / 65535.0f;
    float humidity = 100.0f * (float)raw_humidity / 65535.0f;
    if (!isfinite(temperature) || !isfinite(humidity) ||
        temperature < -40.0f || temperature > 125.0f ||
        humidity < 0.0f || humidity > 100.0f) return false;
    *temperature_c = temperature;
    *humidity_percent = humidity;
    return true;
}
