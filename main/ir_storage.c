#include "ir_storage.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "esp_check.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define IR_SCHEDULE_MAGIC 0x49525335UL
#define IR_SCHEDULE_VERSION 1

static const char *TAG = "ir_storage";
static bool s_exists[IR_CODE_SLOT_COUNT];

const char *ir_protocol_name(ir_protocol_t protocol)
{
    switch (protocol) {
    case IR_PROTOCOL_LG_28_VALID_CHECKSUM: return "LG_28BIT_CHECKSUM_OK";
    case IR_PROTOCOL_LG_LIKE_28: return "LG_LIKE_28BIT_UNCONFIRMED";
    default: return "RAW_UNDECODED";
    }
}

uint32_t ir_code_crc(const ir_code_t *code)
{
    return esp_crc32_le(0, (const uint8_t *)code, offsetof(ir_code_t, crc32));
}

bool ir_code_valid(const ir_code_t *code)
{
    if (!code || code->magic != IR_CODE_MAGIC || code->version != IR_CODE_VERSION ||
        code->duration_count < 4 || code->duration_count > APP_IR_MAX_DURATIONS ||
        code->resolution_hz == 0 || code->carrier_hz < 30000 || code->carrier_hz > 60000 ||
        code->duty_percent < 10 || code->duty_percent > 50 ||
        code->total_duration_us == 0 || code->total_duration_us > APP_IR_MAX_FRAME_US) return false;
    return code->crc32 == ir_code_crc(code);
}

static const char *slot_key(uint8_t slot)
{
    return slot == 0 ? "code0" : "code1";
}

esp_err_t ir_storage_init(void)
{
    esp_err_t err = nvs_flash_init_partition(APP_IR_STORAGE_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(APP_IR_STORAGE_PARTITION), TAG, "erase IR NVS");
        err = nvs_flash_init_partition(APP_IR_STORAGE_PARTITION);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "init IR NVS");
    memset(s_exists, 0, sizeof(s_exists));
    ir_code_t *code = malloc(sizeof(*code));
    if (!code) return ESP_ERR_NO_MEM;
    for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT; ++slot) {
        s_exists[slot] = ir_storage_load_code(slot, code) == ESP_OK;
    }
    free(code);
    ESP_LOGI(TAG, "IR storage ready: slot1=%s slot2=%s", s_exists[0] ? "saved" : "empty",
             s_exists[1] ? "saved" : "empty");
    return ESP_OK;
}

esp_err_t ir_storage_load_code(uint8_t slot, ir_code_t *code)
{
    if (slot >= IR_CODE_SLOT_COUNT || !code) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_from_partition(APP_IR_STORAGE_PARTITION, "ir", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t len = sizeof(*code);
    err = nvs_get_blob(nvs, slot_key(slot), code, &len);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    return len == sizeof(*code) && ir_code_valid(code) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t ir_storage_save_code(uint8_t slot, const ir_code_t *source)
{
    if (slot >= IR_CODE_SLOT_COUNT || !source) return ESP_ERR_INVALID_ARG;
    ir_code_t code = *source;
    code.magic = IR_CODE_MAGIC;
    code.version = IR_CODE_VERSION;
    code.crc32 = ir_code_crc(&code);
    if (!ir_code_valid(&code)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(APP_IR_STORAGE_PARTITION, "ir", NVS_READWRITE, &nvs), TAG, "open IR NVS");
    esp_err_t err = nvs_set_blob(nvs, slot_key(slot), &code, sizeof(code));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_exists[slot] = true;
        ESP_LOGI(TAG, "Saved IR slot %u: durations=%u protocol=%s crc=0x%08lx", slot + 1,
                 code.duration_count, ir_protocol_name(code.protocol), (unsigned long)code.crc32);
    }
    return err;
}

bool ir_storage_code_exists(uint8_t slot)
{
    return slot < IR_CODE_SLOT_COUNT && s_exists[slot];
}

esp_err_t ir_storage_load_schedule(ir_schedule_t *schedule)
{
    if (!schedule) return ESP_ERR_INVALID_ARG;
    memset(schedule, 0, sizeof(*schedule));
    schedule->slot_mask = 1;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_from_partition(APP_IR_STORAGE_PARTITION, "ir", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    size_t len = sizeof(*schedule);
    err = nvs_get_blob(nvs, "schedule", schedule, &len);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(schedule, 0, sizeof(*schedule));
        schedule->slot_mask = 1;
        return ESP_OK;
    }
    if (err != ESP_OK || len != sizeof(*schedule) || schedule->magic != IR_SCHEDULE_MAGIC ||
        schedule->version != IR_SCHEDULE_VERSION || schedule->hour > 23 || schedule->minute > 59 ||
        (schedule->days & 0x80U) || !(schedule->slot_mask & 0x03U) || (schedule->slot_mask & ~0x03U)) {
        memset(schedule, 0, sizeof(*schedule));
        schedule->slot_mask = 1;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t ir_storage_save_schedule(const ir_schedule_t *source)
{
    if (!source || source->enabled > 1 || source->use_light_schedules > 1 || source->hour > 23 ||
        source->minute > 59 || (source->days & 0x80U) || !(source->slot_mask & 0x03U) ||
        (source->slot_mask & ~0x03U)) return ESP_ERR_INVALID_ARG;
    ir_schedule_t schedule = *source;
    schedule.magic = IR_SCHEDULE_MAGIC;
    schedule.version = IR_SCHEDULE_VERSION;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(APP_IR_STORAGE_PARTITION, "ir", NVS_READWRITE, &nvs), TAG, "open IR NVS");
    esp_err_t err = nvs_set_blob(nvs, "schedule", &schedule, sizeof(schedule));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
