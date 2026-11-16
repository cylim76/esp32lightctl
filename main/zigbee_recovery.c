#include "zigbee_recovery.h"

static const int64_t s_retry_delays_ms[ZIGBEE_RECOVERY_MAX_ATTEMPTS] = {
    8000,
    20000,
    60000,
    180000,
};

void zigbee_recovery_begin(zigbee_recovery_state_t *state, int64_t now_ms)
{
    if (!state) return;
    state->active = true;
    state->next_attempt = 0;
    state->started_ms = now_ms;
    state->next_due_ms = now_ms + s_retry_delays_ms[0];
}

void zigbee_recovery_note_response(zigbee_recovery_state_t *state, int64_t response_ms)
{
    if (!state || !state->active || response_ms < state->started_ms) return;
    state->active = false;
}

bool zigbee_recovery_take_due(zigbee_recovery_state_t *state, int64_t now_ms,
                              int64_t last_response_ms, uint8_t *attempt)
{
    if (!state || !state->active) return false;
    if (last_response_ms >= state->started_ms) {
        state->active = false;
        return false;
    }
    if (state->next_attempt >= ZIGBEE_RECOVERY_MAX_ATTEMPTS) {
        state->active = false;
        return false;
    }
    if (now_ms < state->next_due_ms) return false;

    uint8_t current = state->next_attempt++;
    if (attempt) *attempt = current;
    if (state->next_attempt < ZIGBEE_RECOVERY_MAX_ATTEMPTS) {
        state->next_due_ms = state->started_ms + s_retry_delays_ms[state->next_attempt];
    } else {
        state->active = false;
    }
    return true;
}
