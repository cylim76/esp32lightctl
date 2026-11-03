#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "settings.h"

#define SCHEDULER_EXEC_MAGIC 0x45584537UL
#define SCHEDULER_EXEC_VERSION 3

typedef struct {
    uint32_t signature;
    uint32_t date_key;
    time_t due_epoch;
    time_t next_action_epoch;
    uint32_t command_id;
    uint8_t next_retry_index;
    uint8_t attempt_count;
    uint8_t finished;
} scheduler_execution_state_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t index;
    uint16_t payload_size;
    scheduler_execution_state_t state;
    uint32_t checksum;
} scheduler_execution_record_t;

typedef esp_err_t (*scheduler_store_read_fn)(void *ctx, unsigned index, void *data, size_t *size);
typedef esp_err_t (*scheduler_store_write_fn)(void *ctx, unsigned index, const void *data, size_t size);

typedef struct {
    void *ctx;
    scheduler_store_read_fn read;
    scheduler_store_write_fn write;
} scheduler_store_backend_t;

typedef struct {
    scheduler_store_backend_t backend;
    scheduler_execution_state_t persisted[APP_MAX_SCHEDULES];
    bool cached[APP_MAX_SCHEDULES];
    uint32_t writes;
    uint32_t skipped;
    uint32_t failures;
} scheduler_store_t;

void scheduler_store_init(scheduler_store_t *store, const scheduler_store_backend_t *backend);
esp_err_t scheduler_store_load(scheduler_store_t *store, unsigned index,
                               scheduler_execution_state_t *state);
esp_err_t scheduler_store_save(scheduler_store_t *store, unsigned index,
                               const scheduler_execution_state_t *state, bool *changed);
uint32_t scheduler_execution_checksum(const scheduler_execution_record_t *record);
bool scheduler_execution_record_valid(const scheduler_execution_record_t *record, unsigned index);

