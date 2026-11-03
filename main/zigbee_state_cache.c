#include "zigbee_state_cache.h"

#include <string.h>

static bool same_ieee(const uint8_t a[8], const uint8_t b[8])
{
    return memcmp(a, b, ZB_STATE_CACHE_IEEE_LEN) == 0;
}

static zb_state_cache_entry_t *find_by_endpoint(zb_state_cache_t *cache, uint16_t short_addr,
                                                uint8_t endpoint)
{
    if (!cache) return NULL;
    for (size_t i = 0; i < ZB_STATE_CACHE_CAPACITY; ++i) {
        zb_state_cache_entry_t *entry = &cache->entries[i];
        if (entry->used && entry->short_addr == short_addr && entry->endpoint == endpoint) return entry;
    }
    return NULL;
}

static zb_state_cache_entry_t *find_free(zb_state_cache_t *cache)
{
    for (size_t i = 0; i < ZB_STATE_CACHE_CAPACITY; ++i) if (!cache->entries[i].used) return &cache->entries[i];
    return NULL;
}

void zb_state_cache_init(zb_state_cache_t *cache, uint32_t max_age_ms, uint32_t read_timeout_ms)
{
    if (!cache) return;
    memset(cache, 0, sizeof(*cache));
    cache->next_generation = 1;
    cache->max_age_ms = max_age_ms;
    cache->read_timeout_ms = read_timeout_ms;
}

void zb_state_cache_device_announced(zb_state_cache_t *cache, const uint8_t ieee[8],
                                     uint16_t short_addr, int64_t now_ms)
{
    (void)now_ms;
    if (!cache || !ieee) return;
    uint32_t generation = ++cache->next_generation;
    for (size_t i = 0; i < ZB_STATE_CACHE_CAPACITY; ++i) {
        zb_state_cache_entry_t *entry = &cache->entries[i];
        if (!entry->used) continue;
        if (same_ieee(entry->ieee, ieee)) {
            entry->short_addr = short_addr;
            entry->pairing_generation = generation;
            entry->valid = false;
            entry->value = ZB_CACHE_UNKNOWN;
            entry->read_pending = false;
            entry->read_timed_out = false;
            entry->updated_ms = 0;
        } else if (entry->short_addr == short_addr) {
            /* A reused network address must never expose another device's cached state. */
            entry->valid = false;
            entry->value = ZB_CACHE_UNKNOWN;
            entry->read_pending = false;
            entry->read_timed_out = false;
            entry->short_addr = UINT16_MAX;
        }
    }
}

bool zb_state_cache_register_endpoint(zb_state_cache_t *cache, const uint8_t ieee[8],
                                      uint16_t short_addr, uint8_t endpoint)
{
    if (!cache || !ieee || !endpoint) return false;
    zb_state_cache_entry_t *entry = find_by_endpoint(cache, short_addr, endpoint);
    if (entry && !same_ieee(entry->ieee, ieee)) {
        entry->valid = false;
        entry = NULL;
    }
    if (!entry) {
        for (size_t i = 0; i < ZB_STATE_CACHE_CAPACITY; ++i) {
            if (cache->entries[i].used && cache->entries[i].endpoint == endpoint &&
                same_ieee(cache->entries[i].ieee, ieee)) {
                entry = &cache->entries[i];
                break;
            }
        }
    }
    if (!entry) entry = find_free(cache);
    if (!entry) return false;
    if (!entry->used) memset(entry, 0, sizeof(*entry));
    entry->used = true;
    memcpy(entry->ieee, ieee, 8);
    entry->short_addr = short_addr;
    entry->endpoint = endpoint;
    entry->value = ZB_CACHE_UNKNOWN;
    entry->valid = false;
    entry->read_pending = false;
    entry->read_timed_out = false;
    if (!entry->pairing_generation) entry->pairing_generation = cache->next_generation;
    return true;
}

bool zb_state_cache_begin_read(zb_state_cache_t *cache, uint16_t short_addr,
                               uint8_t endpoint, int64_t now_ms)
{
    zb_state_cache_entry_t *entry = find_by_endpoint(cache, short_addr, endpoint);
    if (!entry) return false;
    /* A new read invalidates the old value until a response for this request arrives. */
    entry->valid = false;
    entry->value = ZB_CACHE_UNKNOWN;
    entry->read_pending = true;
    entry->read_timed_out = false;
    entry->read_deadline_ms = now_ms + cache->read_timeout_ms;
    return true;
}

bool zb_state_cache_update(zb_state_cache_t *cache, uint16_t short_addr, uint8_t endpoint,
                           zb_cache_value_t value, int64_t now_ms)
{
    zb_state_cache_entry_t *entry = find_by_endpoint(cache, short_addr, endpoint);
    if (!entry || (value != ZB_CACHE_OFF && value != ZB_CACHE_ON)) return false;
    entry->value = value;
    entry->valid = true;
    entry->updated_ms = now_ms;
    entry->read_pending = false;
    entry->read_timed_out = false;
    entry->read_deadline_ms = 0;
    return true;
}

bool zb_state_cache_get(zb_state_cache_t *cache, uint16_t short_addr, uint8_t endpoint,
                        int64_t now_ms, zb_state_cache_entry_t *out)
{
    zb_state_cache_entry_t *entry = find_by_endpoint(cache, short_addr, endpoint);
    if (!entry) return false;
    if (entry->read_pending && now_ms >= entry->read_deadline_ms) {
        entry->read_pending = false;
        entry->read_timed_out = true;
        entry->valid = false;
        entry->value = ZB_CACHE_UNKNOWN;
    }
    if (entry->valid && (now_ms < entry->updated_ms ||
        (uint64_t)(now_ms - entry->updated_ms) > cache->max_age_ms)) {
        entry->valid = false;
        entry->value = ZB_CACHE_UNKNOWN;
    }
    if (out) *out = *entry;
    return true;
}

void zb_state_cache_invalidate_device(zb_state_cache_t *cache, const uint8_t ieee[8])
{
    if (!cache || !ieee) return;
    for (size_t i = 0; i < ZB_STATE_CACHE_CAPACITY; ++i) {
        zb_state_cache_entry_t *entry = &cache->entries[i];
        if (entry->used && same_ieee(entry->ieee, ieee)) {
            entry->valid = false;
            entry->value = ZB_CACHE_UNKNOWN;
            entry->read_pending = false;
            entry->read_timed_out = false;
            entry->updated_ms = 0;
        }
    }
}
