#pragma once

#define APP_GATEWAY_ENDPOINT          1
#define APP_MAX_ZIGBEE_DEVICES        8
#define APP_MAX_ZIGBEE_ENDPOINTS      16
#define APP_ZIGBEE_CHANNEL            15
#define APP_ZIGBEE_CHANNEL_MASK       (1UL << APP_ZIGBEE_CHANNEL)
#define APP_PAIRING_SECONDS_DEFAULT   180
#define APP_COORDINATOR_MAX_CHILDREN  16
#define APP_ZIGBEE_STATE_MAX_AGE_MS   360000
#define APP_ZIGBEE_READ_TIMEOUT_MS    5000
#define APP_ZIGBEE_STATE_POLL_MS      15000
#define APP_CONTROL_CONFIRM_TIMEOUT_MS 7000
#define APP_IR_RESULT_TIMEOUT_MS       30000
#define APP_CONTROL_COMMAND_CAPACITY  32

#define APP_PROVISION_AP_PREFIX       "LightGW-"
#define APP_PROVISION_AP_PASSWORD     "admin123"
#define APP_AUTH_ITERATIONS           8192
#define APP_AUTH_LEGACY_ITERATIONS    120000
#define APP_AUTH_SALT_LEN             16
#define APP_AUTH_HASH_LEN             32

#define APP_NVS_NAMESPACE             "gateway"
#define APP_RUNTIME_STORAGE_PARTITION "runtime_nvs"
#define APP_TIMEZONE_DEFAULT          "CST-8"

/* DS3231 is optional on every Stage. RTC stores UTC; application uses CST-8. */
#define APP_RTC_I2C_ADDRESS            0x68
#define APP_TIME_MIN_VALID_EPOCH       1793462400LL /* 2026-11-01 00:00:00 CST */
#define APP_TIME_MAX_VALID_EPOCH       4102444799LL /* 2099-12-31 23:59:59 UTC */
#define APP_NTP_DAILY_HOUR_LOCAL       2
#define APP_NTP_RETRY_COUNT            3
#define APP_NTP_WAIT_SECONDS           15

/* Stage 5 RMT pins: header pins without USB/UART/RGB/strapping conflicts. */
#define APP_IR_TX_GPIO                 18
#define APP_IR_RX_GPIO                 19
#define APP_IR_TX_ACTIVE_LEVEL         1
#define APP_IR_RX_MARK_LEVEL           0
#define APP_IR_RESOLUTION_HZ           1000000
#define APP_IR_DEFAULT_CARRIER_HZ      38000
#define APP_IR_DEFAULT_DUTY_PERCENT    33
#define APP_IR_RX_MIN_PULSE_NS         80000
#define APP_IR_RX_END_GAP_NS           25000000
#define APP_IR_MAX_DURATIONS           1024
#define APP_IR_MAX_FRAME_US            300000
#define APP_IR_STORAGE_PARTITION       "ir_data"
#define APP_SENSOR_STORAGE_PARTITION   "sensor_nvs"
#define APP_CLOUD_STORAGE_PARTITION    "cloud_nvs"
#define APP_IR_TASK_HISTORY             16
#define APP_BACKUP_UPLOAD_MIN_INTERVAL_MS 10000

/* Authenticated local-only backup upload limits. */
#define APP_BACKUP_UPLOAD_MIN_INTERVAL_MS 10000

#define AQARA_MODEL_ID                "lumi.switch.l3acn3"
#define AQARA_CLUSTER_LUMI            0xFCC0
#define AQARA_ATTR_MODE               0x0009
#define AQARA_MANUFACTURER_CODE       0x115F
