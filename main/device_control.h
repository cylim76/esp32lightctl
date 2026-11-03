#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "device_state.h"
#include "esp_err.h"
#include "settings.h"

esp_err_t device_control_start(void);
esp_err_t device_control_submit_light(uint16_t short_addr, unsigned channel, bool on,
                                      const char *source, uint32_t *command_id);
esp_err_t device_control_submit_target(schedule_target_t target, const char *source,
                                       uint32_t *command_id);
esp_err_t device_control_submit_target_repeated(schedule_target_t target, uint8_t repeats,
                                                const char *source, uint32_t *command_id);
esp_err_t device_control_get_result(uint32_t command_id, device_command_result_t *result);
