#include "zigbee_nvram_migration.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_zigbee.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MIGRATION_NAMESPACE "zb_migrate"
#define MIGRATION_DONE_KEY  "v2_done"

static const char *TAG = "zigbee_migration";

static esp_err_t legacy_partition_has_data(bool *has_data)
{
    if (!has_data) return ESP_ERR_INVALID_ARG;
    *has_data = false;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        APP_ZIGBEE_LEGACY_PARTITION);
    if (!partition) return ESP_ERR_NOT_FOUND;

    uint8_t block[256];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(block)) {
        size_t length = partition->size - offset;
        if (length > sizeof(block)) length = sizeof(block);
        esp_err_t err = esp_partition_read(partition, offset, block, length);
        if (err != ESP_OK) return err;
        for (size_t i = 0; i < length; ++i) {
            if (block[i] != 0xff) {
                *has_data = true;
                return ESP_OK;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t migration_done(bool *done)
{
    if (!done) return ESP_ERR_INVALID_ARG;
    *done = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MIGRATION_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t value = 0;
    err = nvs_get_u8(handle, MIGRATION_DONE_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err == ESP_OK) *done = value == 1;
    return err;
}

static esp_err_t mark_migration_done(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MIGRATION_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, MIGRATION_DONE_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t zigbee_nvram_prepare(void)
{
    esp_err_t err = nvs_flash_init_partition(APP_ZIGBEE_STORAGE_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Destination %s init failed: %s; no erase attempted",
                 APP_ZIGBEE_STORAGE_PARTITION, esp_err_to_name(err));
        return err;
    }

    bool done = false;
    err = migration_done(&done);
    if (err != ESP_OK) return err;
    if (done) {
        ESP_LOGI(TAG, "Zigbee 2.x NVRAM already prepared in %s",
                 APP_ZIGBEE_STORAGE_PARTITION);
        return ESP_OK;
    }

    bool has_legacy_data = false;
    err = legacy_partition_has_data(&has_legacy_data);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    if (has_legacy_data) {
        const esp_zigbee_nvram_mig_config_t config = {
            .source_partition_name = APP_ZIGBEE_LEGACY_PARTITION,
            .destination_partition_name = APP_ZIGBEE_STORAGE_PARTITION,
            .ieee_addr = NULL,
        };
        ESP_LOGI(TAG, "Migrating legacy Zigbee NVRAM %s -> %s",
                 APP_ZIGBEE_LEGACY_PARTITION, APP_ZIGBEE_STORAGE_PARTITION);
        err = esp_zigbee_nvram_migrate(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Legacy Zigbee NVRAM migration failed: %s; source retained",
                     esp_err_to_name(err));
            return err;
        }
    } else {
        ESP_LOGI(TAG, "No legacy Zigbee NVRAM found; using clean %s",
                 APP_ZIGBEE_STORAGE_PARTITION);
    }

    err = mark_migration_done();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Migration marker commit failed: %s; no partition erased",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Zigbee 2.x NVRAM preparation complete; legacy source retained");
    return ESP_OK;
}
