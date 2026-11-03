#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DEVICE_COMMAND_UNKNOWN = 0,
    DEVICE_COMMAND_PENDING,
    DEVICE_COMMAND_SENT,
    DEVICE_COMMAND_CONFIRMED,
    DEVICE_COMMAND_FAILED,
} device_command_status_t;

typedef enum {
    DEVICE_ACTUAL_UNKNOWN = -1,
    DEVICE_ACTUAL_OFF = 0,
    DEVICE_ACTUAL_ON = 1,
} device_actual_state_t;

typedef struct {
    uint32_t id;
    uint8_t target;
    uint16_t short_addr;
    int8_t channel;
    bool requested_on;
    bool terminal;
    device_command_status_t status;
    device_actual_state_t actual_state;
    int64_t created_ms;
    int64_t sent_ms;
    int64_t completed_ms;
    char source[24];
    char detail[96];
} device_command_result_t;

typedef struct {
    device_command_result_t *records;
    size_t capacity;
    uint32_t next_id;
} device_state_store_t;

void device_state_store_init(device_state_store_t *store, device_command_result_t *records,
                             size_t capacity);
uint32_t device_state_create(device_state_store_t *store, uint8_t target, uint16_t short_addr,
                             int8_t channel, bool requested_on, const char *source, int64_t now_ms);
bool device_state_update(device_state_store_t *store, uint32_t id, device_command_status_t status,
                         device_actual_state_t actual, bool terminal, const char *detail, int64_t now_ms);
bool device_state_get(const device_state_store_t *store, uint32_t id, device_command_result_t *out);
const char *device_command_status_name(device_command_status_t status);
