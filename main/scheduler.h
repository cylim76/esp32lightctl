#pragma once

#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define SCHEDULER_LOG_CAPACITY 8

typedef struct {
    time_t epoch;
    uint8_t schedule_index;
    char message[112];
} scheduler_log_entry_t;

typedef struct {
    time_t next_schedule_epoch;
    time_t last_action_epoch;
    char last_result[128];
    uint8_t log_count;
    scheduler_log_entry_t logs[SCHEDULER_LOG_CAPACITY];
} scheduler_status_t;

esp_err_t scheduler_start(void);
void scheduler_get_status(scheduler_status_t *status);
