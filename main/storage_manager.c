#include "storage_manager.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "stage_config.h"

#define FACTORY_RESET_CONFIRMATION "ERASE_ALL_USER_DATA"

static const char *TAG = "storage";
static storage_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static storage_partition_status_t *find_partition_status(const char *name)
{
    if (!name || strcmp(name, "nvs") == 0) return &s_status.primary;
    if (strcmp(name, APP_RUNTIME_STORAGE_PARTITION) == 0) return &s_status.runtime;
    if (strcmp(name, APP_IR_STORAGE_PARTITION) == 0) return &s_status.ir;
    return NULL;
}

static const char *init_error_reason(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NVS_NO_FREE_PAGES:
        return "no free NVS pages or partition was truncated; automatic erase forbidden";
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return "incompatible/newer NVS format; automatic erase forbidden";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        return "NVS has insufficient space";
    default:
        return esp_err_to_name(err);
    }
}

static void set_error(const char *partition, const char *operation, esp_err_t err)
{
    portENTER_CRITICAL(&s_lock);
    s_status.degraded = true;
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s %s: %s (0x%x)",
             partition ? partition : "nvs", operation, init_error_reason(err), (unsigned)err);
    portEXIT_CRITICAL(&s_lock);
    diagnostics_set_error(TAG, "%s %s failed: %s (0x%x); data retained",
                          partition ? partition : "nvs", operation,
                          init_error_reason(err), (unsigned)err);
    ESP_LOGE(TAG, "%s %s failed: %s (0x%x). No partition was erased.",
             partition ? partition : "nvs", operation, init_error_reason(err), (unsigned)err);
}

static esp_err_t initialize_partition(storage_partition_status_t *status, const char *name,
                                      bool primary)
{
    memset(status, 0, sizeof(*status));
    strlcpy(status->name, name, sizeof(status->name));
    esp_err_t err = primary ? nvs_flash_init() : nvs_flash_init_partition(name);
    status->last_error = err;
    status->initialized = err == ESP_OK;
    if (err != ESP_OK) set_error(name, "init", err);
    return err;
}

static void refresh_one(storage_partition_status_t *status)
{
    if (!status->initialized) return;
    nvs_stats_t stats = {0};
    esp_err_t err = nvs_get_stats(status->name, &stats);
    portENTER_CRITICAL(&s_lock);
    status->last_error = err;
    status->stats_valid = err == ESP_OK;
    if (err == ESP_OK) {
        status->used_entries = stats.used_entries;
        status->free_entries = stats.free_entries;
        status->total_entries = stats.total_entries;
        status->namespace_count = stats.namespace_count;
    }
    portEXIT_CRITICAL(&s_lock);
    if (err != ESP_OK) set_error(status->name, "stats", err);
}

esp_err_t storage_manager_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.last_error, "none", sizeof(s_status.last_error));
    strlcpy(s_status.last_recovery, "none", sizeof(s_status.last_recovery));
    esp_err_t primary_err = initialize_partition(&s_status.primary, "nvs", true);
    esp_err_t runtime_err = initialize_partition(&s_status.runtime, APP_RUNTIME_STORAGE_PARTITION, false);
    if (primary_err == ESP_OK) {
        nvs_handle_t handle;
        if (nvs_open("system", NVS_READONLY, &handle) == ESP_OK) {
            char marker[48] = {0};
            size_t length = sizeof(marker);
            if (nvs_get_str(handle, "last_reset", marker, &length) == ESP_OK) {
                snprintf(s_status.last_recovery, sizeof(s_status.last_recovery),
                         "factory reset marker: %s", marker);
            }
            nvs_close(handle);
        }
    }
    storage_manager_refresh_stats();
    if (primary_err != ESP_OK) return primary_err;
    return runtime_err;
}

esp_err_t storage_manager_init_optional_partition(const char *partition_name)
{
    if (!partition_name) return ESP_ERR_INVALID_ARG;
    storage_partition_status_t *status = find_partition_status(partition_name);
    if (!status) return ESP_ERR_NOT_FOUND;
    if (status->initialized) return ESP_OK;
    esp_err_t err = initialize_partition(status, partition_name, false);
    if (err == ESP_OK) refresh_one(status);
    return err;
}

bool storage_manager_primary_ready(void) { return s_status.primary.initialized; }
bool storage_manager_runtime_ready(void) { return s_status.runtime.initialized; }

void storage_manager_refresh_stats(void)
{
    refresh_one(&s_status.primary);
    refresh_one(&s_status.runtime);
    refresh_one(&s_status.ir);
}

void storage_manager_get_status(storage_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void storage_manager_record_write(const char *partition_name, const char *key,
                                  esp_err_t result, bool changed)
{
    portENTER_CRITICAL(&s_lock);
    if (!changed) {
        s_status.writes_skipped++;
    } else {
        s_status.write_attempts++;
        if (result == ESP_OK) s_status.writes_committed++;
        else s_status.write_failures++;
    }
    portEXIT_CRITICAL(&s_lock);
    if (changed && result != ESP_OK) {
        set_error(partition_name, key ? key : "write", result);
    }
}

void storage_manager_record_corruption(const char *partition_name, const char *detail)
{
    portENTER_CRITICAL(&s_lock);
    s_status.corruption_events++;
    s_status.degraded = true;
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s data invalid: %s",
             partition_name ? partition_name : "nvs", detail ? detail : "unknown");
    portEXIT_CRITICAL(&s_lock);
    diagnostics_set_error(TAG, "%s data invalid: %s; retained for diagnosis",
                          partition_name ? partition_name : "nvs", detail ? detail : "unknown");
}

static esp_err_t erase_raw_partition_if_present(const char *label)
{
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                 ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition) return ESP_OK;
    return esp_partition_erase_range(partition, 0, partition->size);
}

esp_err_t storage_manager_factory_reset(const char *confirmation)
{
    if (!confirmation || strcmp(confirmation, FACTORY_RESET_CONFIRMATION) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(TAG, "USER-CONFIRMED factory reset: erasing user NVS, runtime, IR and Zigbee data");
    diagnostics_record_memory_event("factory_reset_confirmed");

    esp_err_t first_error = ESP_OK;
    nvs_flash_deinit();
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;

    const char *nvs_partitions[] = {APP_RUNTIME_STORAGE_PARTITION, APP_IR_STORAGE_PARTITION};
    for (size_t i = 0; i < sizeof(nvs_partitions) / sizeof(nvs_partitions[0]); ++i) {
        if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
                                      nvs_partitions[i])) continue;
        nvs_flash_deinit_partition(nvs_partitions[i]);
        err = nvs_flash_erase_partition(nvs_partitions[i]);
        if (err != ESP_OK && first_error == ESP_OK) first_error = err;
    }
    err = erase_raw_partition_if_present("zb_storage");
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
    err = erase_raw_partition_if_present("zb_fct");
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;

    if (first_error != ESP_OK) {
        set_error("factory-reset", "erase", first_error);
        return first_error;
    }

    err = nvs_flash_init();
    if (err != ESP_OK) return err;
    nvs_handle_t handle;
    err = nvs_open("system", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "last_reset", "user_confirmed_web");
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.last_recovery, "factory reset explicitly confirmed from Web",
            sizeof(s_status.last_recovery));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "Factory reset completed; reset marker committed");
    return err;
}
