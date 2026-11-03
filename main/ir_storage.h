#pragma once

#include "esp_err.h"
#include "ir_types.h"

esp_err_t ir_storage_init(void);
esp_err_t ir_storage_load_code(uint8_t slot, ir_code_t *code);
esp_err_t ir_storage_save_code(uint8_t slot, const ir_code_t *code);
bool ir_storage_code_exists(uint8_t slot);
esp_err_t ir_storage_load_schedule(ir_schedule_t *schedule);
esp_err_t ir_storage_save_schedule(const ir_schedule_t *schedule);
uint32_t ir_code_crc(const ir_code_t *code);
bool ir_code_valid(const ir_code_t *code);
