#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ir_types.h"

typedef struct {
    bool initialized;
    bool learning;
    bool transmitting;
    bool candidate_ready;
    uint8_t learning_slot;
    uint8_t stable_captures;
    uint8_t attempts;
    bool code_saved[IR_CODE_SLOT_COUNT];
    uint16_t duration_count[IR_CODE_SLOT_COUNT];
    uint32_t carrier_hz[IR_CODE_SLOT_COUNT];
    uint16_t duty_percent[IR_CODE_SLOT_COUNT];
    uint8_t protocol[IR_CODE_SLOT_COUNT];
    uint32_t code_crc[IR_CODE_SLOT_COUNT];
    uint16_t candidate_duration_count;
    uint8_t candidate_protocol;
    uint32_t candidate_total_us;
    uint32_t last_send_epoch;
    uint32_t last_send_uptime_seconds;
    uint8_t last_send_slot_mask;
    char last_send_result[128];
    char schedule_skip_reason[96];
    ir_schedule_t schedule;
} ir_status_t;

typedef enum {
    IR_TASK_UNKNOWN = 0,
    IR_TASK_QUEUED,
    IR_TASK_TRANSMITTING,
    IR_TASK_SENT,
    IR_TASK_FAILED,
} ir_task_state_t;

typedef struct {
    uint32_t id;
    ir_task_state_t state;
    uint8_t slot_mask;
    uint8_t repeats;
    int64_t queued_us;
    int64_t completed_us;
    char origin[24];
    char result[96];
} ir_task_result_t;

esp_err_t ir_manager_start(void);
esp_err_t ir_manager_start_learning(uint8_t slot);
esp_err_t ir_manager_save_candidate(uint8_t slot, uint32_t carrier_hz, uint16_t duty_percent);
esp_err_t ir_manager_submit_send(uint8_t slot_mask, uint8_t repeats, const char *origin,
                                 uint32_t *task_id);
esp_err_t ir_manager_get_task_result(uint32_t task_id, ir_task_result_t *result);
void ir_manager_get_status(ir_status_t *status);
