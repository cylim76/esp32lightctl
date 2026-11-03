#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIAGNOSTICS_MAX_TASKS 48
#define DIAGNOSTICS_MEMORY_EVENT_COUNT 16

typedef struct {
    uint32_t free_heap;
    uint32_t minimum_free_heap;
    uint32_t largest_free_block;
} diagnostics_heap_t;

typedef struct {
    char name[24];
    char state[12];
    uint32_t stack_high_water_words;
    uint32_t stack_high_water_bytes;
    uint32_t priority;
    int core_id;
} diagnostics_task_stat_t;

typedef struct {
    char name[32];
    int64_t uptime_ms;
    diagnostics_heap_t heap;
} diagnostics_memory_event_t;

void diagnostics_init(void);
void diagnostics_set_error(const char *source, const char *format, ...);
void diagnostics_get_last_error(char *out, size_t out_len);
const char *diagnostics_reset_reason(void);
void diagnostics_get_heap(diagnostics_heap_t *heap);
void diagnostics_record_memory_event(const char *name);
void diagnostics_refresh_task_stats(void);
size_t diagnostics_get_task_stats(diagnostics_task_stat_t *out, size_t capacity,
                                  bool *truncated, bool *enabled);
size_t diagnostics_get_memory_events(diagnostics_memory_event_t *out, size_t capacity);
size_t diagnostics_task_count(bool *truncated, bool *enabled);
bool diagnostics_get_task_stat(size_t index, diagnostics_task_stat_t *out);
size_t diagnostics_memory_event_count(void);
bool diagnostics_get_memory_event(size_t index, diagnostics_memory_event_t *out);
