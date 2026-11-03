#pragma once

#define APP_GATEWAY_ENDPOINT          1
#define APP_MAX_ZIGBEE_DEVICES        8
#define APP_MAX_ZIGBEE_ENDPOINTS      16
#define APP_ZIGBEE_CHANNEL            15
#define APP_ZIGBEE_CHANNEL_MASK       (1UL << APP_ZIGBEE_CHANNEL)
#define APP_PAIRING_SECONDS_DEFAULT   180
#define APP_COORDINATOR_MAX_CHILDREN  16

#define APP_PROVISION_AP_PREFIX       "LightGW-"
#define APP_AUTH_ITERATIONS           120000
#define APP_AUTH_SALT_LEN             16
#define APP_AUTH_HASH_LEN             32

#define APP_NVS_NAMESPACE             "gateway"
#define APP_TIMEZONE_DEFAULT          "CST-8"

#define AQARA_MODEL_ID                "lumi.switch.l3acn3"
#define AQARA_CLUSTER_LUMI            0xFCC0
#define AQARA_ATTR_MODE               0x0009
#define AQARA_MANUFACTURER_CODE       0x115F
