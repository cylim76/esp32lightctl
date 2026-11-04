#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device_state.h"
#include "backup_integrity.h"
#include "bh1750_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rainmaker_control_policy.h"
#include "scheduler_persistence.h"
#include "sht31_protocol.h"
#include "zigbee_state_cache.h"

static const uint8_t IEEE_A[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t IEEE_B[8] = {8, 7, 6, 5, 4, 3, 2, 1};
static unsigned s_passed;
/* The production cache is deliberately large; keep it in BSS, not app_main's stack. */
static zb_state_cache_t cache;

typedef struct {
    scheduler_execution_record_t records[APP_MAX_SCHEDULES];
    bool present[APP_MAX_SCHEDULES];
    esp_err_t next_write_error;
    unsigned reads;
    unsigned writes;
} mock_scheduler_nvs_t;

static mock_scheduler_nvs_t mock_nvs;
/* The production store has 56 cached records; keep test instances in BSS so
   fault tests cannot corrupt the relatively small ESP-IDF main-task stack. */
static scheduler_store_t store_a;
static scheduler_store_t store_b;

static esp_err_t mock_scheduler_read(void *ctx, unsigned index, void *data, size_t *size)
{
    mock_scheduler_nvs_t *mock = ctx;
    mock->reads++;
    if (index >= APP_MAX_SCHEDULES || !mock->present[index]) return ESP_ERR_NVS_NOT_FOUND;
    if (!size || *size < sizeof(mock->records[index])) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(data, &mock->records[index], sizeof(mock->records[index]));
    *size = sizeof(mock->records[index]);
    return ESP_OK;
}

static esp_err_t mock_scheduler_write(void *ctx, unsigned index, const void *data, size_t size)
{
    mock_scheduler_nvs_t *mock = ctx;
    mock->writes++;
    if (mock->next_write_error != ESP_OK) {
        esp_err_t err = mock->next_write_error;
        mock->next_write_error = ESP_OK;
        return err;
    }
    if (index >= APP_MAX_SCHEDULES || size != sizeof(mock->records[index])) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&mock->records[index], data, size);
    mock->present[index] = true;
    return ESP_OK;
}

static scheduler_store_backend_t mock_backend(void)
{
    scheduler_store_backend_t backend = {
        .ctx = &mock_nvs,
        .read = mock_scheduler_read,
        .write = mock_scheduler_write,
    };
    return backend;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("PHASE_A_C_TEST FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

#define RUN(test) do { \
    printf("PHASE_A_C_TEST RUN %s\n", #test); \
    test(); \
    s_passed++; \
    printf("PHASE_A_C_TEST PASS %s\n", #test); \
} while (0)

static void test_multi_device_isolation(void)
{
    zb_state_cache_init(&cache, 1000, 100);
    zb_state_cache_device_announced(&cache, IEEE_A, 0x1111, 0);
    zb_state_cache_device_announced(&cache, IEEE_B, 0x2222, 0);
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_A, 0x1111, 1));
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_B, 0x2222, 1));
    CHECK(zb_state_cache_update(&cache, 0x1111, 1, ZB_CACHE_OFF, 10));
    CHECK(zb_state_cache_update(&cache, 0x2222, 1, ZB_CACHE_ON, 20));
    zb_state_cache_entry_t a, b;
    CHECK(zb_state_cache_get(&cache, 0x1111, 1, 30, &a));
    CHECK(zb_state_cache_get(&cache, 0x2222, 1, 30, &b));
    CHECK(a.value == ZB_CACHE_OFF && b.value == ZB_CACHE_ON);
}

static void test_expired_off_is_invalid(void)
{
    zb_state_cache_init(&cache, 1000, 100);
    zb_state_cache_device_announced(&cache, IEEE_A, 0x1111, 0);
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_A, 0x1111, 2));
    CHECK(zb_state_cache_update(&cache, 0x1111, 2, ZB_CACHE_OFF, 10));
    zb_state_cache_entry_t state;
    CHECK(zb_state_cache_get(&cache, 0x1111, 2, 1011, &state));
    CHECK(!state.valid && state.value == ZB_CACHE_UNKNOWN);
}

static void test_read_timeout_rejects_old_off(void)
{
    zb_state_cache_init(&cache, 1000, 100);
    zb_state_cache_device_announced(&cache, IEEE_A, 0x1111, 0);
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_A, 0x1111, 3));
    CHECK(zb_state_cache_update(&cache, 0x1111, 3, ZB_CACHE_OFF, 10));
    CHECK(zb_state_cache_begin_read(&cache, 0x1111, 3, 20));
    zb_state_cache_entry_t state;
    CHECK(zb_state_cache_get(&cache, 0x1111, 3, 20, &state));
    CHECK(!state.valid && state.read_pending);
    CHECK(zb_state_cache_get(&cache, 0x1111, 3, 121, &state));
    CHECK(!state.valid && !state.read_pending && state.read_timed_out);
}

static void test_repair_invalidates_generation(void)
{
    zb_state_cache_init(&cache, 1000, 100);
    zb_state_cache_device_announced(&cache, IEEE_A, 0x1111, 0);
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_A, 0x1111, 1));
    CHECK(zb_state_cache_update(&cache, 0x1111, 1, ZB_CACHE_OFF, 10));
    zb_state_cache_entry_t before, after;
    CHECK(zb_state_cache_get(&cache, 0x1111, 1, 20, &before));
    zb_state_cache_device_announced(&cache, IEEE_A, 0x3333, 30);
    CHECK(!zb_state_cache_update(&cache, 0x1111, 1, ZB_CACHE_OFF, 31));
    CHECK(zb_state_cache_get(&cache, 0x3333, 1, 30, &after));
    CHECK(!after.valid && after.pairing_generation > before.pairing_generation);
}

static void test_fresh_feedback_after_timeout_recovers(void)
{
    zb_state_cache_init(&cache, 1000, 100);
    zb_state_cache_device_announced(&cache, IEEE_A, 0x1111, 0);
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_A, 0x1111, 3));
    CHECK(zb_state_cache_begin_read(&cache, 0x1111, 3, 20));
    zb_state_cache_entry_t state;
    CHECK(zb_state_cache_get(&cache, 0x1111, 3, 120, &state));
    CHECK(state.read_timed_out && !state.valid);
    CHECK(zb_state_cache_update(&cache, 0x1111, 3, ZB_CACHE_OFF, 130));
    CHECK(zb_state_cache_get(&cache, 0x1111, 3, 131, &state));
    CHECK(state.valid && !state.read_timed_out && state.updated_ms == 130);
}

static void test_short_address_reuse_isolated(void)
{
    zb_state_cache_init(&cache, 1000, 100);
    zb_state_cache_device_announced(&cache, IEEE_A, 0x1111, 0);
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_A, 0x1111, 1));
    CHECK(zb_state_cache_update(&cache, 0x1111, 1, ZB_CACHE_OFF, 10));
    zb_state_cache_device_announced(&cache, IEEE_B, 0x1111, 20);
    CHECK(zb_state_cache_register_endpoint(&cache, IEEE_B, 0x1111, 1));
    zb_state_cache_entry_t state;
    CHECK(zb_state_cache_get(&cache, 0x1111, 1, 20, &state));
    CHECK(!state.valid && memcmp(state.ieee, IEEE_B, 8) == 0);
}

static void test_command_lifecycle(void)
{
    device_command_result_t records[4];
    device_state_store_t store;
    device_state_store_init(&store, records, 4);
    uint32_t id = device_state_create(&store, 0, 0x1111, 0, false, "unit", 1);
    CHECK(id != 0);
    device_command_result_t result;
    CHECK(device_state_get(&store, id, &result) && result.status == DEVICE_COMMAND_PENDING);
    CHECK(device_state_update(&store, id, DEVICE_COMMAND_SENT, DEVICE_ACTUAL_UNKNOWN, false, "sent", 2));
    CHECK(device_state_update(&store, id, DEVICE_COMMAND_CONFIRMED, DEVICE_ACTUAL_OFF, true,
                              "fresh feedback", 3));
    CHECK(device_state_get(&store, id, &result));
    CHECK(result.terminal && result.status == DEVICE_COMMAND_CONFIRMED &&
          result.actual_state == DEVICE_ACTUAL_OFF);
}

static void test_command_ring_rejects_inflight_overwrite(void)
{
    device_command_result_t records[4];
    device_state_store_t store;
    device_state_store_init(&store, records, 4);
    uint32_t ids[4];
    for (unsigned i = 0; i < 4; ++i) {
        ids[i] = device_state_create(&store, 0, 0x1111, 0, false, "load", i);
        CHECK(ids[i] != 0);
    }
    CHECK(device_state_create(&store, 0, 0x1111, 0, false, "overflow", 5) == 0);
    CHECK(device_state_update(&store, ids[0], DEVICE_COMMAND_FAILED, DEVICE_ACTUAL_UNKNOWN,
                              true, "done", 6));
    CHECK(device_state_create(&store, 0, 0x1111, 0, false, "reused", 7) != 0);
}

static scheduler_execution_state_t execution_state(unsigned index)
{
    scheduler_execution_state_t state = {
        .signature = 0x1000U + index,
        .date_key = 20261103U,
        .due_epoch = (time_t)(1793680000 + index * 60),
        .next_action_epoch = (time_t)(1793680030 + index * 60),
        .command_id = 200U + index,
        .next_retry_index = (uint8_t)(index % 4),
        .attempt_count = (uint8_t)(index % 3),
        .finished = (uint8_t)(index & 1U),
    };
    return state;
}

static bool execution_states_equal(const scheduler_execution_state_t *a,
                                   const scheduler_execution_state_t *b)
{
    return a->signature == b->signature && a->date_key == b->date_key &&
           a->due_epoch == b->due_epoch && a->next_action_epoch == b->next_action_epoch &&
           a->command_id == b->command_id && a->next_retry_index == b->next_retry_index &&
           a->attempt_count == b->attempt_count && a->finished == b->finished;
}

static void test_scheduler_unchanged_write_is_skipped(void)
{
    memset(&mock_nvs, 0, sizeof(mock_nvs));
    scheduler_store_backend_t backend = mock_backend();
    scheduler_store_init(&store_a, &backend);
    scheduler_execution_state_t state = execution_state(3);
    bool changed = false;
    CHECK(scheduler_store_save(&store_a, 3, &state, &changed) == ESP_OK && changed);
    CHECK(scheduler_store_save(&store_a, 3, &state, &changed) == ESP_OK && !changed);
    CHECK(mock_nvs.writes == 1 && store_a.writes == 1 && store_a.skipped == 1);
}

static void test_scheduler_nvs_full_keeps_last_committed_state(void)
{
    memset(&mock_nvs, 0, sizeof(mock_nvs));
    scheduler_store_backend_t backend = mock_backend();
    scheduler_store_init(&store_a, &backend);
    scheduler_execution_state_t before = execution_state(1);
    scheduler_execution_state_t after = before;
    after.attempt_count++;
    CHECK(scheduler_store_save(&store_a, 1, &before, NULL) == ESP_OK);
    mock_nvs.next_write_error = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    CHECK(scheduler_store_save(&store_a, 1, &after, NULL) == ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    scheduler_store_init(&store_b, &backend);
    scheduler_execution_state_t loaded = {0};
    CHECK(scheduler_store_load(&store_b, 1, &loaded) == ESP_OK);
    CHECK(execution_states_equal(&loaded, &before) && store_a.failures == 1);
}

static void test_scheduler_interrupted_write_recovery(void)
{
    memset(&mock_nvs, 0, sizeof(mock_nvs));
    scheduler_store_backend_t backend = mock_backend();
    scheduler_store_init(&store_a, &backend);
    scheduler_execution_state_t before = execution_state(2);
    scheduler_execution_state_t after = before;
    after.finished = 1;
    CHECK(scheduler_store_save(&store_a, 2, &before, NULL) == ESP_OK);
    mock_nvs.next_write_error = ESP_FAIL;
    CHECK(scheduler_store_save(&store_a, 2, &after, NULL) == ESP_FAIL);
    scheduler_store_init(&store_b, &backend);
    scheduler_execution_state_t loaded = {0};
    CHECK(scheduler_store_load(&store_b, 2, &loaded) == ESP_OK);
    CHECK(execution_states_equal(&loaded, &before));
}

static void test_scheduler_backend_no_mem_keeps_last_committed_state(void)
{
    memset(&mock_nvs, 0, sizeof(mock_nvs));
    scheduler_store_backend_t backend = mock_backend();
    scheduler_store_init(&store_a, &backend);
    scheduler_execution_state_t before = execution_state(5);
    scheduler_execution_state_t after = before;
    after.next_retry_index++;
    CHECK(scheduler_store_save(&store_a, 5, &before, NULL) == ESP_OK);
    mock_nvs.next_write_error = ESP_ERR_NO_MEM;
    CHECK(scheduler_store_save(&store_a, 5, &after, NULL) == ESP_ERR_NO_MEM);
    scheduler_store_init(&store_b, &backend);
    scheduler_execution_state_t loaded = {0};
    CHECK(scheduler_store_load(&store_b, 5, &loaded) == ESP_OK);
    CHECK(execution_states_equal(&loaded, &before) && store_a.failures == 1);
}

static void test_scheduler_corrupt_record_rejected(void)
{
    memset(&mock_nvs, 0, sizeof(mock_nvs));
    scheduler_store_backend_t backend = mock_backend();
    scheduler_store_init(&store_a, &backend);
    scheduler_execution_state_t state = execution_state(4);
    CHECK(scheduler_store_save(&store_a, 4, &state, NULL) == ESP_OK);
    mock_nvs.records[4].checksum ^= 0x1U;
    scheduler_store_init(&store_b, &backend);
    CHECK(scheduler_store_load(&store_b, 4, &state) == ESP_ERR_INVALID_CRC);
}

static void test_scheduler_all_56_entries_restart(void)
{
    memset(&mock_nvs, 0, sizeof(mock_nvs));
    scheduler_store_backend_t backend = mock_backend();
    scheduler_store_init(&store_a, &backend);
    for (unsigned i = 0; i < APP_MAX_SCHEDULES; ++i) {
        scheduler_execution_state_t state = execution_state(i);
        CHECK(scheduler_store_save(&store_a, i, &state, NULL) == ESP_OK);
    }
    CHECK(mock_nvs.writes == APP_MAX_SCHEDULES);
    scheduler_store_init(&store_b, &backend);
    for (unsigned i = 0; i < APP_MAX_SCHEDULES; ++i) {
        scheduler_execution_state_t expected = execution_state(i);
        scheduler_execution_state_t loaded = {0};
        CHECK(scheduler_store_load(&store_b, i, &loaded) == ESP_OK);
        CHECK(execution_states_equal(&loaded, &expected));
    }
}

static void test_scheduler_high_frequency_identical_updates(void)
{
    memset(&mock_nvs, 0, sizeof(mock_nvs));
    scheduler_store_backend_t backend = mock_backend();
    scheduler_store_init(&store_a, &backend);
    scheduler_execution_state_t state = execution_state(0);
    for (unsigned i = 0; i < 1000; ++i) CHECK(scheduler_store_save(&store_a, 0, &state, NULL) == ESP_OK);
    CHECK(mock_nvs.writes == 1 && store_a.skipped == 999);
}

static void test_backup_sha256_and_tamper_detection(void)
{
    static const uint8_t data[] = {'a', 'b', 'c'};
    static const char expected[] = "ba7816bf8f01cfea414140de5dae2223"
                                   "b00361a396177a9cb410ff61f20015ad";
    char digest[BACKUP_SHA256_HEX_LENGTH + 1];
    CHECK(backup_integrity_sha256(data, sizeof(data), digest) == ESP_OK);
    CHECK(strcmp(digest, expected) == 0);
    CHECK(backup_integrity_sha256_matches(data, sizeof(data), expected));
    CHECK(!backup_integrity_sha256_matches((const uint8_t *)"abd", 3, expected));
    CHECK(!backup_integrity_sha256_matches(data, sizeof(data), "bad"));
}

static void test_bh1750_decode_validation_and_filter(void)
{
    float lux = bh1750_protocol_decode_lux(0x01, 0x20);
    CHECK(lux > 239.99f && lux < 240.01f);
    CHECK(bh1750_protocol_lux_valid(lux));
    CHECK(!bh1750_protocol_lux_valid(-0.1f));
    CHECK(!bh1750_protocol_lux_valid(100001.0f));
    float noisy[3] = {240.0f, 9000.0f, 241.0f};
    float filtered = bh1750_protocol_median(noisy, 3);
    CHECK(filtered > 240.99f && filtered < 241.01f);
}

static void test_sht31_crc_and_decode(void)
{
    const uint8_t known[2] = {0xBE, 0xEF};
    CHECK(sht31_protocol_crc8(known, sizeof(known)) == 0x92);
    uint8_t frame[6] = {0x66, 0x66, 0, 0x80, 0x00, 0};
    frame[2] = sht31_protocol_crc8(frame, 2);
    frame[5] = sht31_protocol_crc8(frame + 3, 2);
    float temperature = 0.0f, humidity = 0.0f;
    CHECK(sht31_protocol_decode(frame, &temperature, &humidity));
    CHECK(temperature > 24.9f && temperature < 25.1f);
    CHECK(humidity > 49.9f && humidity < 50.1f);
    frame[5] ^= 1;
    CHECK(!sht31_protocol_decode(frame, &temperature, &humidity));
}

static void test_rainmaker_control_policy(void)
{
    bool on = true;
    CHECK(rainmaker_control_policy_prepare(false, true, true, false, &on) ==
          RAINMAKER_POLICY_IGNORE_INIT);
    CHECK(rainmaker_control_policy_prepare(true, false, false, false, &on) ==
          RAINMAKER_POLICY_IGNORE_RELEASE);
    CHECK(rainmaker_control_policy_prepare(false, true, false, true, &on) ==
          RAINMAKER_POLICY_REJECT_BUSY);
    CHECK(rainmaker_control_policy_prepare(false, true, false, false, &on) ==
          RAINMAKER_POLICY_SUBMIT && on);
    CHECK(rainmaker_control_policy_prepare(false, false, false, false, &on) ==
          RAINMAKER_POLICY_SUBMIT && !on);
    on = true;
    CHECK(rainmaker_control_policy_prepare(true, true, false, false, &on) ==
          RAINMAKER_POLICY_SUBMIT && !on);
}

void app_main(void)
{
    RUN(test_multi_device_isolation);
    RUN(test_expired_off_is_invalid);
    RUN(test_read_timeout_rejects_old_off);
    RUN(test_repair_invalidates_generation);
    RUN(test_fresh_feedback_after_timeout_recovers);
    RUN(test_short_address_reuse_isolated);
    RUN(test_command_lifecycle);
    RUN(test_command_ring_rejects_inflight_overwrite);
    RUN(test_scheduler_unchanged_write_is_skipped);
    RUN(test_scheduler_nvs_full_keeps_last_committed_state);
    RUN(test_scheduler_interrupted_write_recovery);
    RUN(test_scheduler_backend_no_mem_keeps_last_committed_state);
    RUN(test_scheduler_corrupt_record_rejected);
    RUN(test_scheduler_all_56_entries_restart);
    RUN(test_scheduler_high_frequency_identical_updates);
    RUN(test_backup_sha256_and_tamper_detection);
    RUN(test_bh1750_decode_validation_and_filter);
    RUN(test_sht31_crc_and_decode);
    RUN(test_rainmaker_control_policy);
    printf("PHASE_A_C_TEST SUMMARY PASS: %u tests\n", s_passed);
    for (;;) vTaskDelay(portMAX_DELAY);
}
