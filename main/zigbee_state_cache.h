#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZB_STATE_CACHE_IEEE_LEN 8
#define ZB_STATE_CACHE_CAPACITY 128

typedef enum {
    ZB_CACHE_UNKNOWN = -1,
    ZB_CACHE_OFF = 0,
    ZB_CACHE_ON = 1,
} zb_cache_value_t;

typedef struct {
    bool used;
    uint8_t ieee[ZB_STATE_CACHE_IEEE_LEN];
    uint16_t short_addr;
    uint8_t endpoint;
    zb_cache_value_t value;
    bool valid;
    zb_cache_value_t last_known_value;
    bool last_known_valid;
    bool read_pending;
    bool read_timed_out;
    uint16_t consecutive_failures;
    uint32_t pairing_generation;
    int64_t updated_ms;
    int64_t last_known_updated_ms;
    int64_t read_deadline_ms;
} zb_state_cache_entry_t;

typedef struct {
    zb_state_cache_entry_t entries[ZB_STATE_CACHE_CAPACITY];
    uint32_t next_generation;
    uint32_t max_age_ms;
    uint32_t read_timeout_ms;
} zb_state_cache_t;

void zb_state_cache_init(zb_state_cache_t *cache, uint32_t max_age_ms, uint32_t read_timeout_ms);
void zb_state_cache_device_announced(zb_state_cache_t *cache, const uint8_t ieee[8],
                                     uint16_t short_addr, int64_t now_ms);
bool zb_state_cache_register_endpoint(zb_state_cache_t *cache, const uint8_t ieee[8],
                                      uint16_t short_addr, uint8_t endpoint);
bool zb_state_cache_begin_read(zb_state_cache_t *cache, uint16_t short_addr,
                               uint8_t endpoint, int64_t now_ms);
bool zb_state_cache_update(zb_state_cache_t *cache, uint16_t short_addr, uint8_t endpoint,
                           zb_cache_value_t value, int64_t now_ms);
bool zb_state_cache_get(zb_state_cache_t *cache, uint16_t short_addr, uint8_t endpoint,
                        int64_t now_ms, zb_state_cache_entry_t *out);
void zb_state_cache_invalidate_device(zb_state_cache_t *cache, const uint8_t ieee[8]);
