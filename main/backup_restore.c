#include "backup_restore.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "backup_integrity.h"
#include "cJSON.h"
#include "diagnostics.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "extension_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "settings.h"
#include "stage_config.h"
#include "storage_manager.h"
#include "time_manager.h"
#if LIGHTGW_HAS_IR
#include "ir_manager.h"
#include "ir_storage.h"
#endif
#include "ir_types.h"

#define BACKUP_CORE_SCHEMA_VERSION 3
#define BACKUP_TRANSACTION_MAGIC   0x42545831UL
#define BACKUP_TRANSACTION_VERSION 2
#define BACKUP_TRANSACTION_NAMESPACE "backup_tx"
#define BACKUP_TRANSACTION_KEY       "rollback"
#define BACKUP_STAGE_TTL_US (120LL * 1000000LL)

typedef struct {
    uint8_t schedule_count;
    app_schedule_entry_t schedules[APP_MAX_SCHEDULES];
    char timezone[65];
    uint8_t ir_available;
    uint8_t ir_present[IR_CODE_SLOT_COUNT];
    ir_code_t ir_codes[IR_CODE_SLOT_COUNT];
} backup_native_payload_v1_t;

typedef struct {
    uint8_t schedule_count;
    app_schedule_entry_t schedules[APP_MAX_SCHEDULES];
    char timezone[65];
    uint8_t ir_available;
    uint8_t ir_present[IR_CODE_SLOT_COUNT];
    ir_code_t ir_codes[IR_CODE_SLOT_COUNT];
    extension_backup_config_t extensions;
} backup_native_payload_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    backup_native_payload_v1_t previous;
    uint32_t checksum;
} backup_transaction_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    backup_native_payload_t previous;
    uint32_t checksum;
} backup_transaction_t;

typedef struct {
    bool valid;
    int64_t expires_us;
    backup_native_payload_t payload;
    backup_import_summary_t summary;
} backup_staged_t;

static const char *TAG = "backup";
static backup_staged_t *s_staged;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_stage_timer;

static void stage_expired(void *argument)
{
    (void)argument;
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_staged);
    s_staged = NULL;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Validated backup expired and temporary memory was released");
}

static uint32_t bytes_checksum(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t value = 2166136261U;
    for (size_t i = 0; i < length; ++i) value = (value ^ bytes[i]) * 16777619U;
    return value;
}

static bool integer_in_range(const cJSON *item, int minimum, int maximum)
{
    return cJSON_IsNumber(item) && item->valuedouble == item->valueint &&
           item->valueint >= minimum && item->valueint <= maximum;
}

static bool valid_schedule_list(const app_schedule_entry_t *entries, size_t count)
{
    if ((!entries && count) || count > APP_MAX_SCHEDULES) return false;
    uint8_t per_target[APP_SCHEDULE_TARGET_COUNT] = {0};
    for (size_t i = 0; i < count; ++i) {
        const app_schedule_entry_t *entry = &entries[i];
        if (entry->enabled > 1 || entry->target >= APP_SCHEDULE_TARGET_COUNT ||
            entry->hour > 23 || entry->minute > 59 || (entry->days & 0x80U) ||
            entry->retry_count > APP_MAX_SCHEDULE_RETRIES ||
            entry->catchup_window_minutes > 180 ||
            ++per_target[entry->target] > APP_MAX_SCHEDULES_PER_TARGET) return false;
        uint8_t previous = 0;
        for (size_t retry = 0; retry < entry->retry_count; ++retry) {
            uint8_t offset = entry->retry_offsets_minutes[retry];
            if (!offset || offset <= previous || offset > entry->catchup_window_minutes) return false;
            previous = offset;
        }
    }
    return true;
}

static uint32_t local_ir_crc(const ir_code_t *code)
{
    return esp_crc32_le(0, (const uint8_t *)code, offsetof(ir_code_t, crc32));
}

static bool valid_ir_code(const ir_code_t *code)
{
    return code && code->magic == IR_CODE_MAGIC && code->version == IR_CODE_VERSION &&
           code->duration_count >= 4 && code->duration_count <= APP_IR_MAX_DURATIONS &&
           code->resolution_hz && code->carrier_hz >= 30000 && code->carrier_hz <= 60000 &&
           code->duty_percent >= 10 && code->duty_percent <= 50 &&
           code->total_duration_us && code->total_duration_us <= APP_IR_MAX_FRAME_US &&
           code->crc32 == local_ir_crc(code);
}

static esp_err_t snapshot_current(backup_native_payload_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    memset(snapshot, 0, sizeof(*snapshot));
    app_settings_t settings;
    settings_copy(&settings);
    snapshot->schedule_count = settings.schedule_count;
    memcpy(snapshot->schedules, settings.schedules, sizeof(snapshot->schedules));
    strlcpy(snapshot->timezone, settings.timezone, sizeof(snapshot->timezone));
#if LIGHTGW_HAS_IR
    snapshot->ir_available = 1;
    for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT; ++slot) {
        if (ir_storage_load_code(slot, &snapshot->ir_codes[slot]) == ESP_OK) {
            snapshot->ir_present[slot] = 1;
        }
    }
#endif
    return extension_manager_get_backup_config(&snapshot->extensions);
}

static esp_err_t write_primary_snapshot_raw(const backup_native_payload_t *snapshot)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (snapshot->schedule_count) {
        err = nvs_set_blob(nvs, "sched_list", snapshot->schedules,
                           snapshot->schedule_count * sizeof(snapshot->schedules[0]));
    } else {
        err = nvs_erase_key(nvs, "sched_list");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_set_u8(nvs, "sched_ver", 3);
    if (err == ESP_OK) err = nvs_set_str(nvs, "timezone", snapshot->timezone);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write("nvs", "backup_rollback", err, true);
    return err;
}

static esp_err_t write_ir_snapshot_raw(const backup_native_payload_t *snapshot)
{
    if (!snapshot->ir_available) return ESP_OK;
    esp_err_t err = storage_manager_init_optional_partition(APP_IR_STORAGE_PARTITION);
    if (err != ESP_OK) return err;
    nvs_handle_t nvs = 0;
    err = nvs_open_from_partition(APP_IR_STORAGE_PARTITION, "ir", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT && err == ESP_OK; ++slot) {
        const char *key = slot == 0 ? "code0" : "code1";
        if (snapshot->ir_present[slot]) {
            if (!valid_ir_code(&snapshot->ir_codes[slot])) {
                err = ESP_ERR_INVALID_CRC;
                break;
            }
            err = nvs_set_blob(nvs, key, &snapshot->ir_codes[slot], sizeof(ir_code_t));
        } else {
            err = nvs_erase_key(nvs, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        }
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write(APP_IR_STORAGE_PARTITION, "backup_rollback", err, true);
    return err;
}

static esp_err_t restore_snapshot_raw(const backup_native_payload_t *snapshot)
{
    if (!snapshot || !valid_schedule_list(snapshot->schedules, snapshot->schedule_count) ||
        strcmp(snapshot->timezone, APP_TIMEZONE_DEFAULT) != 0) return ESP_ERR_INVALID_STATE;
    esp_err_t err = write_primary_snapshot_raw(snapshot);
    if (err == ESP_OK) err = write_ir_snapshot_raw(snapshot);
    if (err == ESP_OK && snapshot->extensions.present_mask) {
        size_t applied = 0;
        size_t skipped = 0;
        bool restart_required = false;
        err = extension_manager_apply_backup_config(&snapshot->extensions, &applied, &skipped,
                                                    &restart_required);
    }
    return err;
}

static esp_err_t journal_save(const backup_native_payload_t *previous)
{
    if (!storage_manager_runtime_ready()) return ESP_ERR_INVALID_STATE;
    backup_transaction_t *record = calloc(1, sizeof(*record));
    if (!record) return ESP_ERR_NO_MEM;
    record->magic = BACKUP_TRANSACTION_MAGIC;
    record->version = BACKUP_TRANSACTION_VERSION;
    record->previous = *previous;
    record->checksum = bytes_checksum(record, offsetof(backup_transaction_t, checksum));
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open_from_partition(APP_RUNTIME_STORAGE_PARTITION,
                                             BACKUP_TRANSACTION_NAMESPACE,
                                             NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_blob(nvs, BACKUP_TRANSACTION_KEY, record, sizeof(*record));
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (err == ESP_OK) ESP_LOGW(TAG, "Import rollback journal committed before configuration writes");
    if (nvs) nvs_close(nvs);
    free(record);
    storage_manager_record_write(APP_RUNTIME_STORAGE_PARTITION, "backup_journal", err, true);
    return err;
}

static esp_err_t journal_clear(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open_from_partition(APP_RUNTIME_STORAGE_PARTITION,
                                             BACKUP_TRANSACTION_NAMESPACE,
                                             NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, BACKUP_TRANSACTION_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    storage_manager_record_write(APP_RUNTIME_STORAGE_PARTITION, "backup_journal_clear", err, true);
    return err;
}

esp_err_t backup_restore_recover_pending(void)
{
    if (!storage_manager_runtime_ready()) return ESP_ERR_INVALID_STATE;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open_from_partition(APP_RUNTIME_STORAGE_PARTITION,
                                             BACKUP_TRANSACTION_NAMESPACE,
                                             NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t *raw = calloc(1, sizeof(backup_transaction_t));
    if (!raw) { nvs_close(nvs); return ESP_ERR_NO_MEM; }
    size_t length = sizeof(backup_transaction_t);
    err = nvs_get_blob(nvs, BACKUP_TRANSACTION_KEY, raw, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) { free(raw); return ESP_OK; }
    backup_native_payload_t previous = {0};
    bool valid = false;
    if (err == ESP_OK && length == sizeof(backup_transaction_t)) {
        const backup_transaction_t *record = (const backup_transaction_t *)raw;
        valid = record->magic == BACKUP_TRANSACTION_MAGIC &&
                record->version == BACKUP_TRANSACTION_VERSION &&
                record->checksum == bytes_checksum(record, offsetof(backup_transaction_t, checksum));
        if (valid) previous = record->previous;
    } else if (err == ESP_OK && length == sizeof(backup_transaction_v1_t)) {
        const backup_transaction_v1_t *record = (const backup_transaction_v1_t *)raw;
        valid = record->magic == BACKUP_TRANSACTION_MAGIC && record->version == 1 &&
                record->checksum == bytes_checksum(record, offsetof(backup_transaction_v1_t, checksum));
        if (valid) {
            previous.schedule_count = record->previous.schedule_count;
            memcpy(previous.schedules, record->previous.schedules, sizeof(previous.schedules));
            strlcpy(previous.timezone, record->previous.timezone, sizeof(previous.timezone));
            previous.ir_available = record->previous.ir_available;
            memcpy(previous.ir_present, record->previous.ir_present, sizeof(previous.ir_present));
            memcpy(previous.ir_codes, record->previous.ir_codes, sizeof(previous.ir_codes));
        }
    }
    if (!valid) {
        free(raw);
        storage_manager_record_corruption(APP_RUNTIME_STORAGE_PARTITION,
                                          "backup rollback journal invalid; retained for diagnosis");
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGW(TAG, "Interrupted configuration import detected; restoring pre-import snapshot");
    err = restore_snapshot_raw(&previous);
    free(raw);
    if (err == ESP_OK) err = journal_clear();
    if (err == ESP_OK) diagnostics_set_error(TAG, "Interrupted import rolled back; original configuration restored");
    return err;
}

static cJSON *schedule_to_json(const app_schedule_entry_t *entry)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddBoolToObject(item, "enabled", entry->enabled != 0);
    cJSON_AddNumberToObject(item, "target", entry->target);
    cJSON_AddNumberToObject(item, "hour", entry->hour);
    cJSON_AddNumberToObject(item, "minute", entry->minute);
    cJSON_AddNumberToObject(item, "days", entry->days);
    cJSON_AddNumberToObject(item, "retry_count", entry->retry_count);
    cJSON *offsets = cJSON_AddArrayToObject(item, "retry_offsets_minutes");
    for (size_t i = 0; i < entry->retry_count; ++i) {
        cJSON_AddItemToArray(offsets, cJSON_CreateNumber(entry->retry_offsets_minutes[i]));
    }
    cJSON_AddNumberToObject(item, "catchup_window_minutes", entry->catchup_window_minutes);
    return item;
}

static esp_err_t add_ir_export(cJSON *payload)
{
    cJSON *ir = cJSON_AddObjectToObject(payload, "lg_ir");
#if LIGHTGW_HAS_IR
    cJSON_AddTrueToObject(ir, "available_in_source_firmware");
    cJSON *codes = cJSON_AddArrayToObject(ir, "off_codes");
    for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT; ++slot) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "slot", slot);
        ir_code_t *code = calloc(1, sizeof(*code));
        bool present = code && ir_storage_load_code(slot, code) == ESP_OK;
        cJSON_AddBoolToObject(item, "present", present);
        if (present) {
            size_t encoded_capacity = 4 * ((sizeof(*code) + 2) / 3) + 1;
            unsigned char *encoded = calloc(1, encoded_capacity);
            size_t encoded_length = 0;
            if (!encoded || mbedtls_base64_encode(encoded, encoded_capacity, &encoded_length,
                                                  (const unsigned char *)code, sizeof(*code)) != 0) {
                free(encoded);
                free(code);
                cJSON_Delete(item);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(item, "encoding", "base64-ir-code-v1");
            cJSON_AddNumberToObject(item, "byte_length", sizeof(*code));
            cJSON_AddNumberToObject(item, "duration_count", code->duration_count);
            cJSON_AddNumberToObject(item, "crc32", code->crc32);
            cJSON_AddStringToObject(item, "data", (const char *)encoded);
            free(encoded);
        }
        free(code);
        cJSON_AddItemToArray(codes, item);
    }
#else
    cJSON_AddFalseToObject(ir, "available_in_source_firmware");
    cJSON_AddArrayToObject(ir, "off_codes");
#endif
    cJSON_AddStringToObject(ir, "command_semantics", "explicit_off_only");
    return ESP_OK;
}

static cJSON *add_extension(cJSON *extensions, const char *name, bool included, int version)
{
    cJSON *item = cJSON_AddObjectToObject(extensions, name);
    cJSON_AddBoolToObject(item, "included_in_source_firmware", included);
    cJSON_AddNumberToObject(item, "config_version", version);
    cJSON *settings = cJSON_AddObjectToObject(item, "non_sensitive_settings");
    cJSON_AddFalseToObject(item, "credentials_included");
    return settings;
}

esp_err_t backup_restore_export_json(char **json_out, size_t *length_out,
                                     char *filename, size_t filename_size)
{
    if (!json_out || !length_out || !filename || filename_size < 24) return ESP_ERR_INVALID_ARG;
    *json_out = NULL;
    *length_out = 0;
    diagnostics_record_memory_event("backup_export_begin");
    cJSON *root = cJSON_CreateObject();
    cJSON *document = cJSON_AddObjectToObject(root, "document");
    cJSON_AddNumberToObject(document, "format_version", BACKUP_FORMAT_VERSION);
    cJSON_AddStringToObject(document, "product", "esp32lightctl");
    cJSON_AddStringToObject(document, "device_type", "ESP32-C6-DevKitC-1-N8");
    cJSON_AddStringToObject(document, "firmware", LIGHTGW_VERSION);
    cJSON_AddStringToObject(document, "source_variant", LIGHTGW_VARIANT_NAME);
    time_manager_status_t time_status;
    time_manager_get_status(&time_status);
    char created[32] = "UNAVAILABLE";
    char stamp[20] = "unknown-time";
    if (time_status.valid) {
        struct tm utc;
        gmtime_r(&time_status.now_utc, &utc);
        strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &utc);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc);
    }
    cJSON_AddStringToObject(document, "created_at_utc", created);
    cJSON_AddBoolToObject(document, "created_time_valid", time_status.valid);
    cJSON_AddFalseToObject(document, "sensitive_data_included");
    cJSON *payload = cJSON_AddObjectToObject(document, "payload");
    cJSON_AddNumberToObject(payload, "core_schema_version", BACKUP_CORE_SCHEMA_VERSION);
    cJSON_AddStringToObject(payload, "target", "esp32c6");
    cJSON_AddStringToObject(payload, "timezone", APP_TIMEZONE_DEFAULT);
    cJSON *lighting = cJSON_AddObjectToObject(payload, "lighting");
    cJSON *channels = cJSON_AddArrayToObject(lighting, "channels");
    cJSON_AddItemToArray(channels, cJSON_CreateString("left"));
    cJSON_AddItemToArray(channels, cJSON_CreateString("center"));
    cJSON_AddItemToArray(channels, cJSON_CreateString("right"));
    cJSON_AddFalseToObject(lighting, "zigbee_network_included");
    cJSON_AddFalseToObject(lighting, "device_mapping_included");
    cJSON_AddTrueToObject(lighting, "repair_and_rediscovery_required");
    app_settings_t settings;
    settings_copy(&settings);
    cJSON *schedules = cJSON_AddArrayToObject(payload, "schedules");
    for (size_t i = 0; i < settings.schedule_count; ++i) {
        cJSON_AddItemToArray(schedules, schedule_to_json(&settings.schedules[i]));
    }
    esp_err_t err = add_ir_export(payload);
    cJSON *extensions = cJSON_AddObjectToObject(payload, "extensions");
    extension_backup_config_t extension_config;
    esp_err_t extension_err = extension_manager_get_backup_config(&extension_config);
    if (extension_err != ESP_OK) {
        cJSON_Delete(root);
        return extension_err;
    }
    cJSON *bh = add_extension(extensions, "bh1750", LIGHTGW_HAS_BH1750_EXTENSION, 1);
    if (LIGHTGW_HAS_BH1750_EXTENSION) {
        cJSON_AddBoolToObject(bh, "enabled", (extension_config.enabled_mask &
                              (1U << EXTENSION_MODULE_BH1750)) != 0);
        cJSON_AddNumberToObject(bh, "sample_period_seconds", extension_config.bh1750_sample_period_seconds);
        cJSON_AddNumberToObject(bh, "night_start_minute", extension_config.bh1750_night_start_minute);
        cJSON_AddNumberToObject(bh, "night_end_minute", extension_config.bh1750_night_end_minute);
        cJSON_AddNumberToObject(bh, "night_lux_threshold", extension_config.bh1750_night_lux_threshold);
        cJSON_AddNumberToObject(bh, "consecutive_anomaly_limit", extension_config.bh1750_consecutive_anomaly_limit);
        cJSON_AddNumberToObject(bh, "calibration_valid_mask", extension_config.bh1750_calibration_valid_mask);
        cJSON *calibration = cJSON_AddArrayToObject(bh, "calibration_lux");
        for (size_t i = 0; i < 5; ++i) {
            cJSON_AddItemToArray(calibration, cJSON_CreateNumber(extension_config.bh1750_calibration_lux[i]));
        }
    }
    cJSON *sht = add_extension(extensions, "sht31", LIGHTGW_HAS_SHT31_EXTENSION, 1);
    if (LIGHTGW_HAS_SHT31_EXTENSION) {
        cJSON_AddBoolToObject(sht, "enabled", (extension_config.enabled_mask &
                              (1U << EXTENSION_MODULE_SHT31)) != 0);
        cJSON_AddNumberToObject(sht, "sample_period_seconds", extension_config.sht31_sample_period_seconds);
    }
    cJSON *rainmaker = add_extension(extensions, "rainmaker", LIGHTGW_HAS_RAINMAKER_EXTENSION, 1);
    if (LIGHTGW_HAS_RAINMAKER_EXTENSION) {
        cJSON_AddBoolToObject(rainmaker, "enabled", (extension_config.enabled_mask &
                              (1U << EXTENSION_MODULE_RAINMAKER)) != 0);
    }
    cJSON *excluded = cJSON_AddArrayToObject(payload, "excluded_sensitive_or_identity_data");
    const char *excluded_names[] = {"wifi_password", "admin_password", "rainmaker_credentials",
                                    "zigbee_network_keys", "zigbee_pairing_data", "zigbee_device_mappings"};
    for (size_t i = 0; i < sizeof(excluded_names) / sizeof(excluded_names[0]); ++i) {
        cJSON_AddItemToArray(excluded, cJSON_CreateString(excluded_names[i]));
    }
    char *canonical = err == ESP_OK ? cJSON_PrintUnformatted(document) : NULL;
    char digest[BACKUP_SHA256_HEX_LENGTH + 1];
    if (!canonical || backup_integrity_sha256((const uint8_t *)canonical, strlen(canonical), digest) != ESP_OK) {
        err = ESP_ERR_NO_MEM;
    }
    free(canonical);
    if (err == ESP_OK) {
        cJSON *integrity = cJSON_AddObjectToObject(root, "integrity");
        cJSON_AddStringToObject(integrity, "algorithm", "SHA-256");
        cJSON_AddStringToObject(integrity, "scope", "canonical-document-json");
        cJSON_AddStringToObject(integrity, "digest", digest);
        *json_out = cJSON_PrintUnformatted(root);
        if (!*json_out) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        *length_out = strlen(*json_out);
        if (*length_out > BACKUP_MAX_FILE_BYTES) {
            free(*json_out);
            *json_out = NULL;
            *length_out = 0;
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    snprintf(filename, filename_size, "esp32lightctl-backup-v%d-%s.json",
             BACKUP_FORMAT_VERSION, stamp);
    cJSON_Delete(root);
    diagnostics_record_memory_event("backup_export_end");
    return err;
}

static bool parse_schedule(const cJSON *item, app_schedule_entry_t *entry)
{
    cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
    cJSON *target = cJSON_GetObjectItem(item, "target");
    cJSON *hour = cJSON_GetObjectItem(item, "hour");
    cJSON *minute = cJSON_GetObjectItem(item, "minute");
    cJSON *days = cJSON_GetObjectItem(item, "days");
    cJSON *retry_count = cJSON_GetObjectItem(item, "retry_count");
    cJSON *offsets = cJSON_GetObjectItem(item, "retry_offsets_minutes");
    cJSON *window = cJSON_GetObjectItem(item, "catchup_window_minutes");
    if (!cJSON_IsBool(enabled) || !integer_in_range(target, 0, APP_SCHEDULE_TARGET_COUNT - 1) ||
        !integer_in_range(hour, 0, 23) || !integer_in_range(minute, 0, 59) ||
        !integer_in_range(days, 0, 127) ||
        !integer_in_range(retry_count, 0, APP_MAX_SCHEDULE_RETRIES) ||
        !cJSON_IsArray(offsets) || cJSON_GetArraySize(offsets) != retry_count->valueint ||
        !integer_in_range(window, 0, 180)) return false;
    memset(entry, 0, sizeof(*entry));
    entry->enabled = cJSON_IsTrue(enabled);
    entry->target = target->valueint;
    entry->hour = hour->valueint;
    entry->minute = minute->valueint;
    entry->days = days->valueint;
    entry->retry_count = retry_count->valueint;
    entry->catchup_window_minutes = window->valueint;
    for (size_t i = 0; i < entry->retry_count; ++i) {
        cJSON *offset = cJSON_GetArrayItem(offsets, i);
        if (!integer_in_range(offset, 1, 180)) return false;
        entry->retry_offsets_minutes[i] = offset->valueint;
    }
    return true;
}

static bool target_includes_extension(extension_module_id_t module)
{
    switch (module) {
    case EXTENSION_MODULE_BH1750: return LIGHTGW_HAS_BH1750_EXTENSION;
    case EXTENSION_MODULE_SHT31: return LIGHTGW_HAS_SHT31_EXTENSION;
    case EXTENSION_MODULE_RAINMAKER: return LIGHTGW_HAS_RAINMAKER_EXTENSION;
    default: return false;
    }
}

static bool parse_extension_sections(const cJSON *extensions,
                                     extension_backup_config_t *config,
                                     size_t *section_count, size_t *skipped_count)
{
    if (!cJSON_IsObject(extensions) || !config || !section_count || !skipped_count) return false;
    const char *names[] = {"bh1750", "sht31", "rainmaker"};
    memset(config, 0, sizeof(*config));
    *section_count = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        cJSON *item = cJSON_GetObjectItem(extensions, names[i]);
        if (!item) continue;
        cJSON *included = cJSON_GetObjectItem(item, "included_in_source_firmware");
        cJSON *version = cJSON_GetObjectItem(item, "config_version");
        cJSON *settings = cJSON_GetObjectItem(item, "non_sensitive_settings");
        cJSON *credentials = cJSON_GetObjectItem(item, "credentials_included");
        if (!cJSON_IsObject(item) || !cJSON_IsBool(included) ||
            !integer_in_range(version, 1, 1) ||
            !cJSON_IsObject(settings) || !cJSON_IsFalse(credentials)) return false;
        char *bounded = cJSON_PrintUnformatted(settings);
        bool valid = bounded && strlen(bounded) <= 1024;
        free(bounded);
        if (!valid) return false;
        (*section_count)++;
        if (!cJSON_IsTrue(included)) continue;
        /* Backward compatibility: original v1 files exported empty extension
         * placeholders. They remain valid and never overwrite target data. */
        if (cJSON_GetArraySize(settings) == 0) {
            (*skipped_count)++;
            continue;
        }
        cJSON *enabled = cJSON_GetObjectItem(settings, "enabled");
        if (!cJSON_IsBool(enabled)) return false;
        extension_module_id_t module = (extension_module_id_t)i;
        config->present_mask |= 1U << module;
        if (cJSON_IsTrue(enabled)) config->enabled_mask |= 1U << module;
        if (module == EXTENSION_MODULE_BH1750) {
            cJSON *sample = cJSON_GetObjectItem(settings, "sample_period_seconds");
            cJSON *night_start = cJSON_GetObjectItem(settings, "night_start_minute");
            cJSON *night_end = cJSON_GetObjectItem(settings, "night_end_minute");
            cJSON *threshold = cJSON_GetObjectItem(settings, "night_lux_threshold");
            cJSON *consecutive = cJSON_GetObjectItem(settings, "consecutive_anomaly_limit");
            cJSON *mask = cJSON_GetObjectItem(settings, "calibration_valid_mask");
            cJSON *calibration = cJSON_GetObjectItem(settings, "calibration_lux");
            if (!integer_in_range(sample, 10, 300) || !integer_in_range(night_start, 0, 1439) ||
                !integer_in_range(night_end, 0, 1439) || !cJSON_IsNumber(threshold) ||
                !isfinite(threshold->valuedouble) || threshold->valuedouble < 0.0 ||
                threshold->valuedouble > 10000.0 || !integer_in_range(consecutive, 1, 20) ||
                !integer_in_range(mask, 0, 0x1F) || !cJSON_IsArray(calibration) ||
                cJSON_GetArraySize(calibration) != 5) return false;
            config->bh1750_sample_period_seconds = sample->valueint;
            config->bh1750_night_start_minute = night_start->valueint;
            config->bh1750_night_end_minute = night_end->valueint;
            config->bh1750_night_lux_threshold = threshold->valuedouble;
            config->bh1750_consecutive_anomaly_limit = consecutive->valueint;
            config->bh1750_calibration_valid_mask = mask->valueint;
            for (size_t calibration_index = 0; calibration_index < 5; ++calibration_index) {
                cJSON *value = cJSON_GetArrayItem(calibration, calibration_index);
                if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
                    value->valuedouble < 0.0 || value->valuedouble > 100000.0) return false;
                config->bh1750_calibration_lux[calibration_index] = value->valuedouble;
            }
        } else if (module == EXTENSION_MODULE_SHT31) {
            cJSON *sample = cJSON_GetObjectItem(settings, "sample_period_seconds");
            if (!integer_in_range(sample, 30, 60)) return false;
            config->sht31_sample_period_seconds = sample->valueint;
        }
        if (!target_includes_extension(module)) (*skipped_count)++;
    }
    return true;
}

static bool parse_ir(const cJSON *ir, backup_native_payload_t *payload, size_t *code_count)
{
    if (!cJSON_IsObject(ir)) return false;
    cJSON *available = cJSON_GetObjectItem(ir, "available_in_source_firmware");
    cJSON *codes = cJSON_GetObjectItem(ir, "off_codes");
    cJSON *semantics = cJSON_GetObjectItem(ir, "command_semantics");
    if (!cJSON_IsBool(available) || !cJSON_IsArray(codes) ||
        !cJSON_IsString(semantics) || strcmp(semantics->valuestring, "explicit_off_only") != 0 ||
        cJSON_GetArraySize(codes) > IR_CODE_SLOT_COUNT) return false;
    payload->ir_available = cJSON_IsTrue(available);
    *code_count = 0;
    uint8_t seen = 0;
    for (int i = 0; i < cJSON_GetArraySize(codes); ++i) {
        cJSON *item = cJSON_GetArrayItem(codes, i);
        cJSON *slot = cJSON_GetObjectItem(item, "slot");
        cJSON *present = cJSON_GetObjectItem(item, "present");
        if (!cJSON_IsObject(item) || !integer_in_range(slot, 0, IR_CODE_SLOT_COUNT - 1) ||
            !cJSON_IsBool(present) || (seen & (1U << slot->valueint))) return false;
        seen |= 1U << slot->valueint;
        if (!cJSON_IsTrue(present)) continue;
        cJSON *encoding = cJSON_GetObjectItem(item, "encoding");
        cJSON *length = cJSON_GetObjectItem(item, "byte_length");
        cJSON *data = cJSON_GetObjectItem(item, "data");
        if (!payload->ir_available || !cJSON_IsString(encoding) ||
            strcmp(encoding->valuestring, "base64-ir-code-v1") != 0 ||
            !integer_in_range(length, sizeof(ir_code_t), sizeof(ir_code_t)) || !cJSON_IsString(data)) return false;
        size_t decoded_length = 0;
        if (mbedtls_base64_decode((unsigned char *)&payload->ir_codes[slot->valueint],
                                  sizeof(ir_code_t), &decoded_length,
                                  (const unsigned char *)data->valuestring,
                                  strlen(data->valuestring)) != 0 || decoded_length != sizeof(ir_code_t) ||
            !valid_ir_code(&payload->ir_codes[slot->valueint])) return false;
        payload->ir_present[slot->valueint] = 1;
        (*code_count)++;
    }
    return true;
}

esp_err_t backup_restore_validate_import(const char *json, size_t length,
                                         backup_import_summary_t *summary)
{
    if (!json || !summary || !length || length > BACKUP_MAX_FILE_BYTES) return ESP_ERR_INVALID_SIZE;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    if (!s_stage_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = stage_expired,
            .name = "backup_stage_ttl",
        };
        if (esp_timer_create(&timer_args, &s_stage_timer) != ESP_OK) return ESP_ERR_NO_MEM;
    }
    diagnostics_record_memory_event("backup_import_validate_begin");
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &parse_end, false);
    if (root && parse_end) {
        while (parse_end < json + length && isspace((unsigned char)*parse_end)) parse_end++;
        if (parse_end != json + length) {
            cJSON_Delete(root);
            root = NULL;
        }
    }
    cJSON *document = root ? cJSON_GetObjectItem(root, "document") : NULL;
    cJSON *integrity = root ? cJSON_GetObjectItem(root, "integrity") : NULL;
    cJSON *version = document ? cJSON_GetObjectItem(document, "format_version") : NULL;
    cJSON *product = document ? cJSON_GetObjectItem(document, "product") : NULL;
    cJSON *device_type = document ? cJSON_GetObjectItem(document, "device_type") : NULL;
    cJSON *sensitive = document ? cJSON_GetObjectItem(document, "sensitive_data_included") : NULL;
    cJSON *payload_json = document ? cJSON_GetObjectItem(document, "payload") : NULL;
    cJSON *algorithm = integrity ? cJSON_GetObjectItem(integrity, "algorithm") : NULL;
    cJSON *scope = integrity ? cJSON_GetObjectItem(integrity, "scope") : NULL;
    cJSON *digest = integrity ? cJSON_GetObjectItem(integrity, "digest") : NULL;
    esp_err_t err = ESP_OK;
    if (!cJSON_IsObject(root) || !cJSON_IsObject(document) || !cJSON_IsObject(integrity) ||
        !integer_in_range(version, BACKUP_FORMAT_VERSION, BACKUP_FORMAT_VERSION) ||
        !cJSON_IsString(product) || strcmp(product->valuestring, "esp32lightctl") != 0 ||
        !cJSON_IsString(device_type) || strcmp(device_type->valuestring, "ESP32-C6-DevKitC-1-N8") != 0 ||
        !cJSON_IsFalse(sensitive) || !cJSON_IsObject(payload_json) ||
        !cJSON_IsString(algorithm) || strcmp(algorithm->valuestring, "SHA-256") != 0 ||
        !cJSON_IsString(scope) || strcmp(scope->valuestring, "canonical-document-json") != 0 ||
        !cJSON_IsString(digest)) err = ESP_ERR_INVALID_ARG;
    char *canonical = err == ESP_OK ? cJSON_PrintUnformatted(document) : NULL;
    if (err == ESP_OK && (!canonical || !backup_integrity_sha256_matches(
            (const uint8_t *)canonical, strlen(canonical), digest->valuestring))) err = ESP_ERR_INVALID_CRC;
    free(canonical);

    backup_native_payload_t *candidate = calloc(1, sizeof(*candidate));
    backup_import_summary_t parsed = {.requires_zigbee_repair = true};
    if (!candidate) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        cJSON *schema = cJSON_GetObjectItem(payload_json, "core_schema_version");
        cJSON *target = cJSON_GetObjectItem(payload_json, "target");
        cJSON *timezone = cJSON_GetObjectItem(payload_json, "timezone");
        cJSON *lighting = cJSON_GetObjectItem(payload_json, "lighting");
        cJSON *network = lighting ? cJSON_GetObjectItem(lighting, "zigbee_network_included") : NULL;
        cJSON *mapping = lighting ? cJSON_GetObjectItem(lighting, "device_mapping_included") : NULL;
        cJSON *schedules = cJSON_GetObjectItem(payload_json, "schedules");
        if (!integer_in_range(schema, BACKUP_CORE_SCHEMA_VERSION, BACKUP_CORE_SCHEMA_VERSION) ||
            !cJSON_IsString(target) || strcmp(target->valuestring, "esp32c6") != 0 ||
            !cJSON_IsString(timezone) || strcmp(timezone->valuestring, APP_TIMEZONE_DEFAULT) != 0 ||
            !cJSON_IsObject(lighting) || !cJSON_IsFalse(network) || !cJSON_IsFalse(mapping) ||
            !cJSON_IsArray(schedules) || cJSON_GetArraySize(schedules) > APP_MAX_SCHEDULES) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            strlcpy(candidate->timezone, timezone->valuestring, sizeof(candidate->timezone));
            candidate->schedule_count = cJSON_GetArraySize(schedules);
            for (size_t i = 0; i < candidate->schedule_count && err == ESP_OK; ++i) {
                if (!parse_schedule(cJSON_GetArrayItem(schedules, i), &candidate->schedules[i])) {
                    err = ESP_ERR_INVALID_ARG;
                }
            }
            if (err == ESP_OK && !valid_schedule_list(candidate->schedules, candidate->schedule_count)) {
                err = ESP_ERR_INVALID_ARG;
            }
        }
    }
    if (err == ESP_OK) {
        cJSON *ir = cJSON_GetObjectItem(payload_json, "lg_ir");
        if (!parse_ir(ir, candidate, &parsed.ir_codes)) err = ESP_ERR_INVALID_ARG;
        parsed.ir_section_available = candidate->ir_available;
    }
    if (err == ESP_OK) {
        cJSON *extensions = cJSON_GetObjectItem(payload_json, "extensions");
        parsed.skipped_sections = 1; /* Zigbee pairing/network/mapping. */
        if (!parse_extension_sections(extensions, &candidate->extensions,
                                      &parsed.extension_sections,
                                      &parsed.skipped_sections)) err = ESP_ERR_NOT_SUPPORTED;
        for (extension_module_id_t module = 0; module < EXTENSION_MODULE_COUNT; ++module) {
            if ((candidate->extensions.present_mask & (1U << module)) &&
                target_includes_extension(module)) parsed.restored_extensions++;
        }
#if !LIGHTGW_HAS_IR
        if (candidate->ir_available) parsed.skipped_sections++;
#endif
        parsed.schedules = candidate->schedule_count;
    }
    if (err == ESP_OK) {
        backup_staged_t *stage = calloc(1, sizeof(*stage));
        if (!stage) err = ESP_ERR_NO_MEM;
        if (stage) {
            stage->valid = true;
            stage->expires_us = esp_timer_get_time() + BACKUP_STAGE_TTL_US;
            stage->payload = *candidate;
            stage->summary = parsed;
            strlcpy(stage->summary.token, digest->valuestring, sizeof(stage->summary.token));
            snprintf(stage->summary.message, sizeof(stage->summary.message),
                     "validated: %u schedules, %u IR OFF codes, %u extension setting section(s); Zigbee re-pair required",
                     (unsigned)parsed.schedules, (unsigned)parsed.ir_codes,
                     (unsigned)parsed.restored_extensions);
        }
        if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_staged);
        s_staged = stage;
        *summary = s_staged->summary;
        xSemaphoreGive(s_lock);
        esp_timer_stop(s_stage_timer);
        esp_timer_start_once(s_stage_timer, BACKUP_STAGE_TTL_US);
        }
    } else {
        backup_restore_discard_staged();
    }
    free(candidate);
    cJSON_Delete(root);
    diagnostics_record_memory_event("backup_import_validate_end");
    return err;
}

void backup_restore_discard_staged(void)
{
    if (!s_lock) return;
    if (s_stage_timer) esp_timer_stop(s_stage_timer);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_staged);
    s_staged = NULL;
    xSemaphoreGive(s_lock);
}

static esp_err_t apply_import(const backup_native_payload_t *payload)
{
    esp_err_t err = settings_save_schedules(payload->schedules, payload->schedule_count,
                                             payload->timezone);
#if LIGHTGW_HAS_IR
    if (err == ESP_OK && payload->ir_available) {
        for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT && err == ESP_OK; ++slot) {
            err = payload->ir_present[slot] ? ir_storage_save_code(slot, &payload->ir_codes[slot]) :
                                             ir_storage_delete_code(slot);
        }
    }
#endif
    if (err == ESP_OK && payload->extensions.present_mask) {
        size_t applied = 0;
        size_t skipped = 0;
        bool restart_required = false;
        err = extension_manager_apply_backup_config(&payload->extensions, &applied, &skipped,
                                                    &restart_required);
    }
    return err;
}

esp_err_t backup_restore_commit_import(const char *token,
                                       backup_import_summary_t *summary)
{
    if (!token || !summary || !s_lock) return ESP_ERR_INVALID_ARG;
#if LIGHTGW_HAS_IR
    if (ir_manager_storage_busy()) return ESP_ERR_INVALID_STATE;
#endif
    backup_native_payload_t *candidate = calloc(1, sizeof(*candidate));
    backup_native_payload_t *previous = calloc(1, sizeof(*previous));
    if (!candidate || !previous) {
        free(candidate);
        free(previous);
        return ESP_ERR_NO_MEM;
    }
    backup_import_summary_t staged_summary;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool valid = s_staged && s_staged->valid && esp_timer_get_time() <= s_staged->expires_us &&
                 strcmp(token, s_staged->summary.token) == 0;
    if (valid) {
        *candidate = s_staged->payload;
        staged_summary = s_staged->summary;
    }
    xSemaphoreGive(s_lock);
    if (!valid) {
        free(candidate);
        free(previous);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = snapshot_current(previous);
    if (err == ESP_OK) err = journal_save(previous);
    if (err != ESP_OK) {
        free(candidate);
        free(previous);
        return err;
    }
    diagnostics_record_memory_event("backup_import_commit_begin");
    err = apply_import(candidate);
#if LIGHTGW_HAS_IR
    if (err == ESP_OK) err = ir_manager_refresh_storage();
#endif
    if (err == ESP_OK) err = journal_clear();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Import failed (%s); restoring pre-import configuration", esp_err_to_name(err));
        esp_err_t rollback_err = apply_import(previous);
#if LIGHTGW_HAS_IR
        if (rollback_err == ESP_OK) rollback_err = ir_manager_refresh_storage();
#endif
        if (rollback_err == ESP_OK) rollback_err = journal_clear();
        if (rollback_err != ESP_OK) {
            diagnostics_set_error(TAG, "Import and immediate rollback failed; boot recovery journal retained: %s",
                                  esp_err_to_name(rollback_err));
        }
        free(candidate);
        free(previous);
        return err;
    }
    diagnostics_record_memory_event("backup_import_commit_end");
    snprintf(staged_summary.message, sizeof(staged_summary.message),
             "restored %u schedules, %u IR OFF codes and %u extension section(s); skipped %u section(s); re-pair Zigbee devices",
             (unsigned)staged_summary.schedules,
#if LIGHTGW_HAS_IR
             (unsigned)staged_summary.ir_codes,
#else
             0U,
#endif
             (unsigned)staged_summary.restored_extensions,
             (unsigned)staged_summary.skipped_sections);
    *summary = staged_summary;
    free(candidate);
    free(previous);
    backup_restore_discard_staged();
    ESP_LOGI(TAG, "%s", summary->message);
    return ESP_OK;
}
