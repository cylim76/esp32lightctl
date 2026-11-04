#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

float bh1750_protocol_decode_lux(uint8_t msb, uint8_t lsb);
bool bh1750_protocol_lux_valid(float lux);
float bh1750_protocol_median(const float *values, size_t count);
