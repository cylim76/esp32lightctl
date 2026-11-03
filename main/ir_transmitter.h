#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "ir_types.h"

esp_err_t ir_transmitter_init(void);
esp_err_t ir_transmitter_send(const ir_code_t *code, uint8_t repeats, uint32_t repeat_gap_ms);
