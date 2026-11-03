#include "device_state.h"

#include <string.h>

static void copy_text(char *out, size_t capacity, const char *text)
{
    if (!capacity) return;
    if (!text) text = "";
    size_t length = strlen(text);
    if (length >= capacity) length = capacity - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

void device_state_store_init(device_state_store_t *store, device_command_result_t *records,
                             size_t capacity)
{
    if (!store) return;
    store->records = records;
    store->capacity = capacity;
    store->next_id = 0;
    if (records && capacity) memset(records, 0, capacity * sizeof(*records));
}

uint32_t device_state_create(device_state_store_t *store, uint8_t target, uint16_t short_addr,
                             int8_t channel, bool requested_on, const char *source, int64_t now_ms)
{
    if (!store || !store->records || !store->capacity) return 0;
    uint32_t id = store->next_id + 1;
    if (!id) id = 1;
    device_command_result_t *record = &store->records[id % store->capacity];
    /* Never evict an in-flight result just because the bounded history wrapped. */
    if (record->id && !record->terminal) return 0;
    store->next_id = id;
    memset(record, 0, sizeof(*record));
    record->id = id;
    record->target = target;
    record->short_addr = short_addr;
    record->channel = channel;
    record->requested_on = requested_on;
    record->status = DEVICE_COMMAND_PENDING;
    record->actual_state = DEVICE_ACTUAL_UNKNOWN;
    record->created_ms = now_ms;
    copy_text(record->source, sizeof(record->source), source);
    copy_text(record->detail, sizeof(record->detail), "queued");
    return id;
}

bool device_state_update(device_state_store_t *store, uint32_t id, device_command_status_t status,
                         device_actual_state_t actual, bool terminal, const char *detail, int64_t now_ms)
{
    if (!store || !store->records || !id) return false;
    device_command_result_t *record = &store->records[id % store->capacity];
    if (record->id != id) return false;
    record->status = status;
    record->actual_state = actual;
    record->terminal = terminal;
    if (status == DEVICE_COMMAND_SENT && !record->sent_ms) record->sent_ms = now_ms;
    if (terminal) record->completed_ms = now_ms;
    copy_text(record->detail, sizeof(record->detail), detail);
    return true;
}

bool device_state_get(const device_state_store_t *store, uint32_t id, device_command_result_t *out)
{
    if (!store || !store->records || !id || !out) return false;
    const device_command_result_t *record = &store->records[id % store->capacity];
    if (record->id != id) return false;
    *out = *record;
    return true;
}

const char *device_command_status_name(device_command_status_t status)
{
    switch (status) {
    case DEVICE_COMMAND_PENDING: return "PENDING";
    case DEVICE_COMMAND_SENT: return "SENT";
    case DEVICE_COMMAND_CONFIRMED: return "CONFIRMED";
    case DEVICE_COMMAND_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}
