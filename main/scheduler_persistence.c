#include "scheduler_persistence.h"

#include <stddef.h>
#include <string.h>

static bool execution_state_equal(const scheduler_execution_state_t *a,
                                  const scheduler_execution_state_t *b)
{
    return a->signature == b->signature && a->date_key == b->date_key &&
           a->due_epoch == b->due_epoch && a->next_action_epoch == b->next_action_epoch &&
           a->command_id == b->command_id && a->next_retry_index == b->next_retry_index &&
           a->attempt_count == b->attempt_count && a->finished == b->finished;
}

static void execution_state_copy_canonical(scheduler_execution_state_t *dst,
                                           const scheduler_execution_state_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->signature = src->signature;
    dst->date_key = src->date_key;
    dst->due_epoch = src->due_epoch;
    dst->next_action_epoch = src->next_action_epoch;
    dst->command_id = src->command_id;
    dst->next_retry_index = src->next_retry_index;
    dst->attempt_count = src->attempt_count;
    dst->finished = src->finished;
}

uint32_t scheduler_execution_checksum(const scheduler_execution_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < offsetof(scheduler_execution_record_t, checksum); ++i) {
        hash = (hash ^ bytes[i]) * 16777619UL;
    }
    return hash;
}

bool scheduler_execution_record_valid(const scheduler_execution_record_t *record, unsigned index)
{
    return record && index < APP_MAX_SCHEDULES && record->magic == SCHEDULER_EXEC_MAGIC &&
           record->version == SCHEDULER_EXEC_VERSION && record->index == index &&
           record->payload_size == sizeof(record->state) &&
           record->checksum == scheduler_execution_checksum(record);
}

void scheduler_store_init(scheduler_store_t *store, const scheduler_store_backend_t *backend)
{
    if (!store) return;
    memset(store, 0, sizeof(*store));
    if (backend) store->backend = *backend;
}

esp_err_t scheduler_store_load(scheduler_store_t *store, unsigned index,
                               scheduler_execution_state_t *state)
{
    if (!store || !state || index >= APP_MAX_SCHEDULES || !store->backend.read) {
        return ESP_ERR_INVALID_ARG;
    }
    scheduler_execution_record_t record = {0};
    size_t size = sizeof(record);
    esp_err_t err = store->backend.read(store->backend.ctx, index, &record, &size);
    if (err != ESP_OK) return err;
    if (size != sizeof(record) || !scheduler_execution_record_valid(&record, index)) {
        return ESP_ERR_INVALID_CRC;
    }
    execution_state_copy_canonical(state, &record.state);
    execution_state_copy_canonical(&store->persisted[index], &record.state);
    store->cached[index] = true;
    return ESP_OK;
}

esp_err_t scheduler_store_save(scheduler_store_t *store, unsigned index,
                               const scheduler_execution_state_t *state, bool *changed)
{
    if (changed) *changed = false;
    if (!store || !state || index >= APP_MAX_SCHEDULES || !store->backend.write) {
        return ESP_ERR_INVALID_ARG;
    }
    if (store->cached[index] && execution_state_equal(&store->persisted[index], state)) {
        store->skipped++;
        return ESP_OK;
    }
    scheduler_execution_record_t record = {0};
    record.magic = SCHEDULER_EXEC_MAGIC;
    record.version = SCHEDULER_EXEC_VERSION;
    record.index = index;
    record.payload_size = sizeof(record.state);
    execution_state_copy_canonical(&record.state, state);
    record.checksum = scheduler_execution_checksum(&record);
    esp_err_t err = store->backend.write(store->backend.ctx, index, &record, sizeof(record));
    if (err != ESP_OK) {
        store->failures++;
        return err;
    }
    execution_state_copy_canonical(&store->persisted[index], state);
    store->cached[index] = true;
    store->writes++;
    if (changed) *changed = true;
    return ESP_OK;
}
