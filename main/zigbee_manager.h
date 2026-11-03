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
    bool online;
    uint16_t short_addr;
    esp_zb_ieee_addr_t ieee_addr;
    char model[33];
    uint8_t endpoints[3];
    zb_channel_state_t state[3];
    int8_t last_rssi;
} zb_device_snapshot_t;

esp_err_t zigbee_manager_start(void);
bool zigbee_manager_network_ready(void);
uint8_t zigbee_manager_channel(void);
uint16_t zigbee_manager_pan_id(void);
int zigbee_manager_pairing_remaining(void);
esp_err_t zigbee_manager_permit_join(uint8_t seconds);
esp_err_t zigbee_manager_set_channel(uint16_t short_addr, unsigned channel, bool on);
esp_err_t zigbee_manager_all_off(void);
size_t zigbee_manager_snapshot(zb_device_snapshot_t *out, size_t capacity);
