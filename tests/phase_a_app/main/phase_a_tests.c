#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device_state.h"
#include "zigbee_state_cache.h"

static const uint8_t IEEE_A[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t IEEE_B[8] = {8, 7, 6, 5, 4, 3, 2, 1};
static unsigned s_passed;
/* The production cache is deliberately large; keep it in BSS, not app_main's stack. */
static zb_state_cache_t cache;

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
    printf("PHASE_A_C_TEST SUMMARY PASS: %u tests\n", s_passed);
}
