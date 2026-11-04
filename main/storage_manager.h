#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_PARTITION_NAME_MAX 16
#define STORAGE_ERROR_TEXT_MAX 128

typedef struct {
    char name[STORAGE_PARTITION_NAME_MAX];
    bool initialized;
    bool stats_valid;
    uint32_t used_entries;
    uint32_t free_entries;
    uint32_t total_entries;
    uint32_t namespace_count;
    esp_err_t last_error;
} storage_partition_status_t;

typedef struct {
    storage_partition_status_t primary;
    storage_partition_status_t runtime;
    storage_partition_status_t ir;
    storage_partition_status_t sensor;
    storage_partition_status_t cloud;
    uint32_t write_attempts;
    uint32_t writes_committed;
    uint32_t writes_skipped;
    uint32_t write_failures;
    uint32_t corruption_events;
    bool degraded;
    char last_error[STORAGE_ERROR_TEXT_MAX];
    char last_recovery[STORAGE_ERROR_TEXT_MAX];
} storage_status_t;

/* Initializes default and runtime NVS without ever erasing them automatically. */
esp_err_t storage_manager_init(void);
esp_err_t storage_manager_init_optional_partition(const char *partition_name);
bool storage_manager_primary_ready(void);
bool storage_manager_runtime_ready(void);
void storage_manager_refresh_stats(void);
void storage_manager_get_status(storage_status_t *status);
void storage_manager_record_write(const char *partition_name, const char *key,
                                  esp_err_t result, bool changed);
void storage_manager_record_corruption(const char *partition_name, const char *detail);

/* Destructive and only called by the explicitly confirmed Web factory-reset API. */
esp_err_t storage_manager_factory_reset(const char *confirmation);
