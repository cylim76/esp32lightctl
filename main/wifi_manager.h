#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t wifi_manager_start(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_provisioning(void);
bool wifi_manager_time_is_synced(void);
void wifi_manager_get_ip(char *out, size_t out_len);
const char *wifi_manager_provision_ssid(void);
