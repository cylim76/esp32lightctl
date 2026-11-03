#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "ir_types.h"

esp_err_t ir_receiver_open(void);
esp_err_t ir_receiver_capture(ir_code_t *code, uint32_t timeout_ms);
void ir_receiver_close(void);
