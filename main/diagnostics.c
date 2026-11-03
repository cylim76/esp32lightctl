#include "diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_lock;
static char s_last_error[160] = "none";
static const char *s_reset_reason = "unknown";
static diagnostics_task_stat_t s_tasks[DIAGNOSTICS_MAX_TASKS];
static size_t s_task_count;
static bool s_tasks_truncated;
static diagnostics_memory_event_t s_events[DIAGNOSTICS_MEMORY_EVENT_COUNT];
static size_t s_event_count;
static size_t s_event_next;

static void resource_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        diagnostics_refresh_task_stats();
        diagnostics_heap_t heap;
        diagnostics_get_heap(&heap);
        bool truncated = false, enabled = false;
        size_t count = diagnostics_task_count(&truncated, &enabled);
        uint32_t lowest_bytes = UINT32_MAX;
        char lowest_name[24] = "none";
        for (size_t i = 0; i < count; ++i) {
            diagnostics_task_stat_t stat;
            if (diagnostics_get_task_stat(i, &stat) && stat.stack_high_water_bytes < lowest_bytes) {
                lowest_bytes = stat.stack_high_water_bytes;
                strlcpy(lowest_name, stat.name, sizeof(lowest_name));
            }
        }
        ESP_LOGI("resource", "free=%lu min=%lu largest=%lu tasks=%u%s lowest_stack=%s/%luB",
                 (unsigned long)heap.free_heap, (unsigned long)heap.minimum_free_heap,
                 (unsigned long)heap.largest_free_block, (unsigned)count,
                 truncated ? "+" : "", lowest_name,
                 (unsigned long)(lowest_bytes == UINT32_MAX ? 0 : lowest_bytes));
        if (enabled && lowest_bytes < 512) {
            ESP_LOGW("resource", "Low stack high-water mark: task=%s remaining=%lu bytes",
                     lowest_name, (unsigned long)lowest_bytes);
        }
    }
}

static const char *task_state_name(eTaskState state)
{
    switch (state) {
    case eRunning: return "running";
    case eReady: return "ready";
    case eBlocked: return "blocked";
    case eSuspended: return "suspended";
    case eDeleted: return "deleted";
    case eInvalid: return "invalid";
    default: return "unknown";
    }
}

void diagnostics_get_heap(diagnostics_heap_t *heap)
{
    if (!heap) return;
    heap->free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    heap->minimum_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    heap->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

void diagnostics_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: s_reset_reason = "power_on"; break;
    case ESP_RST_SW: s_reset_reason = "software"; break;
    case ESP_RST_PANIC: s_reset_reason = "panic"; break;
    case ESP_RST_INT_WDT: s_reset_reason = "interrupt_watchdog"; break;
    case ESP_RST_TASK_WDT: s_reset_reason = "task_watchdog"; break;
    case ESP_RST_WDT: s_reset_reason = "watchdog"; break;
    case ESP_RST_BROWNOUT: s_reset_reason = "brownout"; break;
    case ESP_RST_DEEPSLEEP: s_reset_reason = "deep_sleep"; break;
    default: s_reset_reason = "other"; break;
    }
    diagnostics_record_memory_event("boot");
    if (xTaskCreate(resource_monitor_task, "resource_monitor", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE("diagnostics", "Failed to start resource monitor task");
    }
}

void diagnostics_set_error(const char *source, const char *format, ...)
{
    char detail[112];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_last_error, sizeof(s_last_error), "%s: %s", source ? source : "app", detail);
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGE(source ? source : "diagnostics", "%s", detail);
}

void diagnostics_get_last_error(char *out, size_t out_len)
{
    if (!out || !out_len) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_last_error, out_len);
    if (s_lock) xSemaphoreGive(s_lock);
}

const char *diagnostics_reset_reason(void)
{
    return s_reset_reason;
}

void diagnostics_record_memory_event(const char *name)
{
    diagnostics_memory_event_t event = {0};
    strlcpy(event.name, name ? name : "unnamed", sizeof(event.name));
    event.uptime_ms = esp_timer_get_time() / 1000;
    diagnostics_get_heap(&event.heap);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_events[s_event_next] = event;
    s_event_next = (s_event_next + 1) % DIAGNOSTICS_MEMORY_EVENT_COUNT;
    if (s_event_count < DIAGNOSTICS_MEMORY_EVENT_COUNT) s_event_count++;
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGI("memory", "event=%s free=%lu min=%lu largest=%lu",
             event.name, (unsigned long)event.heap.free_heap,
             (unsigned long)event.heap.minimum_free_heap,
             (unsigned long)event.heap.largest_free_block);
}

void diagnostics_refresh_task_stats(void)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    static TaskStatus_t snapshot[DIAGNOSTICS_MAX_TASKS];
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    UBaseType_t expected = uxTaskGetNumberOfTasks();
    bool truncated = expected > DIAGNOSTICS_MAX_TASKS;
    UBaseType_t count = truncated ? 0 : uxTaskGetSystemState(snapshot, DIAGNOSTICS_MAX_TASKS, NULL);
    s_task_count = count;
    s_tasks_truncated = truncated || (expected && !count);
    for (size_t i = 0; i < count; ++i) {
        strlcpy(s_tasks[i].name, snapshot[i].pcTaskName ? snapshot[i].pcTaskName : "unknown",
                sizeof(s_tasks[i].name));
        strlcpy(s_tasks[i].state, task_state_name(snapshot[i].eCurrentState), sizeof(s_tasks[i].state));
        s_tasks[i].stack_high_water_words = snapshot[i].usStackHighWaterMark;
        s_tasks[i].stack_high_water_bytes = snapshot[i].usStackHighWaterMark * sizeof(StackType_t);
        s_tasks[i].priority = snapshot[i].uxCurrentPriority;
#if CONFIG_FREERTOS_SMP
        s_tasks[i].core_id = snapshot[i].xCoreID;
#else
        s_tasks[i].core_id = 0;
#endif
    }
    if (s_lock) xSemaphoreGive(s_lock);
#endif
}

size_t diagnostics_get_task_stats(diagnostics_task_stat_t *out, size_t capacity,
                                  bool *truncated, bool *enabled)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    if (enabled) *enabled = true;
#else
    if (enabled) *enabled = false;
#endif
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_task_count < capacity ? s_task_count : capacity;
    if (out && count) memcpy(out, s_tasks, count * sizeof(*out));
    if (truncated) *truncated = s_tasks_truncated || (out && s_task_count > capacity);
    if (s_lock) xSemaphoreGive(s_lock);
    return out ? count : s_task_count;
}

size_t diagnostics_get_memory_events(diagnostics_memory_event_t *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_event_count < capacity ? s_event_count : capacity;
    size_t first = (s_event_next + DIAGNOSTICS_MEMORY_EVENT_COUNT - s_event_count) %
                   DIAGNOSTICS_MEMORY_EVENT_COUNT;
    if (s_event_count > count) first = (first + s_event_count - count) % DIAGNOSTICS_MEMORY_EVENT_COUNT;
    for (size_t i = 0; i < count; ++i) {
        out[i] = s_events[(first + i) % DIAGNOSTICS_MEMORY_EVENT_COUNT];
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return count;
}

size_t diagnostics_task_count(bool *truncated, bool *enabled)
{
    return diagnostics_get_task_stats(NULL, 0, truncated, enabled);
}

bool diagnostics_get_task_stat(size_t index, diagnostics_task_stat_t *out)
{
    if (!out) return false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = index < s_task_count;
    if (found) *out = s_tasks[index];
    if (s_lock) xSemaphoreGive(s_lock);
    return found;
}

size_t diagnostics_memory_event_count(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_event_count;
    if (s_lock) xSemaphoreGive(s_lock);
    return count;
}

bool diagnostics_get_memory_event(size_t index, diagnostics_memory_event_t *out)
{
    if (!out) return false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = index < s_event_count;
    if (found) {
        size_t first = (s_event_next + DIAGNOSTICS_MEMORY_EVENT_COUNT - s_event_count) %
                       DIAGNOSTICS_MEMORY_EVENT_COUNT;
        *out = s_events[(first + index) % DIAGNOSTICS_MEMORY_EVENT_COUNT];
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return found;
}
