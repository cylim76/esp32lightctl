#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_zigbee_core.h"

typedef enum {
    ZB_CHANNEL_UNKNOWN = -1,
    ZB_CHANNEL_OFF = 0,
    ZB_CHANNEL_ON = 1,
} zb_channel_state_t;

typedef struct {
    bool used;
    bool supported;
    bool mapping_confirmed;
    bool online;
    uint16_t short_addr;
    esp_zb_ieee_addr_t ieee_addr;
    char model[33];
    uint8_t endpoints[3];
    uint8_t discovered_endpoint_count;
    uint8_t discovered_endpoints[16];
    uint8_t onoff_endpoint_count;
    uint8_t onoff_endpoints[16];
    zb_channel_state_t state[3];
    int8_t last_rssi;
} zb_device_snapshot_t;

typedef struct {
    bool known_endpoint;
    bool valid;
    bool read_pending;
    bool read_timed_out;
    bool last_known_valid;
    zb_channel_state_t state;
    zb_channel_state_t last_known_state;
    uint8_t endpoint;
    uint16_t consecutive_failures;
    int64_t updated_us;
    int64_t last_known_updated_us;
} zb_channel_status_t;

esp_err_t zigbee_manager_start(void);
bool zigbee_manager_network_ready(void);
uint8_t zigbee_manager_channel(void);
uint16_t zigbee_manager_pan_id(void);
int zigbee_manager_pairing_remaining(void);
esp_err_t zigbee_manager_permit_join(uint8_t seconds);
esp_err_t zigbee_manager_diagnostic_read(uint16_t short_addr);
esp_err_t zigbee_manager_set_channel(uint16_t short_addr, unsigned channel, bool on);
esp_err_t zigbee_manager_confirm_mapping(uint16_t short_addr, const uint8_t endpoints[3]);
size_t zigbee_manager_snapshot(zb_device_snapshot_t *out, size_t capacity);
esp_err_t zigbee_manager_get_channel_status(uint16_t short_addr, unsigned channel,
                                            zb_channel_status_t *status);
esp_err_t zigbee_manager_get_endpoint_status(uint16_t short_addr, uint8_t endpoint,
                                             zb_channel_status_t *status);
