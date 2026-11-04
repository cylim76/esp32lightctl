#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t sht31_protocol_crc8(const uint8_t *data, size_t length);
bool sht31_protocol_decode(const uint8_t frame[6], float *temperature_c,
                           float *humidity_percent);
