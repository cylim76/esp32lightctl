#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

#define IR_CODE_MAGIC          0x49524335UL
#define IR_CODE_VERSION        1
#define IR_CODE_SLOT_COUNT     2
#define IR_CAPTURE_REQUIRED    3

typedef enum {
    IR_PROTOCOL_RAW = 0,
    IR_PROTOCOL_LG_LIKE_28,
    IR_PROTOCOL_LG_28_VALID_CHECKSUM,
} ir_protocol_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t duration_count;
    uint32_t resolution_hz;
    uint32_t carrier_hz;
    uint16_t duty_percent;
    uint8_t mark_level;
    uint8_t carrier_measured;
    uint8_t protocol;
    uint8_t reserved;
    uint32_t decoded_value;
    uint32_t total_duration_us;
    uint16_t durations_us[APP_IR_MAX_DURATIONS];
    uint32_t crc32;
} ir_code_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t enabled;
    uint8_t use_light_schedules;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    uint8_t slot_mask;
    uint8_t reserved;
} ir_schedule_t;

const char *ir_protocol_name(ir_protocol_t protocol);
