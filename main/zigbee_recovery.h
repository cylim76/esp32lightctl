#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZIGBEE_RECOVERY_MAX_ATTEMPTS 4

typedef struct {
    bool active;
    uint8_t next_attempt;
    int64_t started_ms;
    int64_t next_due_ms;
} zigbee_recovery_state_t;

void zigbee_recovery_begin(zigbee_recovery_state_t *state, int64_t now_ms);
void zigbee_recovery_note_response(zigbee_recovery_state_t *state, int64_t response_ms);
bool zigbee_recovery_take_due(zigbee_recovery_state_t *state, int64_t now_ms,
                              int64_t last_response_ms, uint8_t *attempt);
