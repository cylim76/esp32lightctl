#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RAINMAKER_EXTENSION_CONFIG_NAMESPACE "rainmaker_ext"
#define RAINMAKER_EXTENSION_CONFIG_VERSION   1
#define RAINMAKER_NODE_ID_LENGTH             40

typedef enum {
    RAINMAKER_COMMAND_PENDING = 0,
    RAINMAKER_COMMAND_CONFIRMED,
    RAINMAKER_COMMAND_SENT_UNCONFIRMED,
    RAINMAKER_COMMAND_FAILED,
} rainmaker_command_state_t;

typedef struct {
    bool valid;
    bool on;
    int64_t updated_uptime_ms;
} rainmaker_light_state_t;

typedef struct {
    rainmaker_command_state_t state;
    bool requested_on;
    char detail[96];
} rainmaker_command_result_t;

typedef struct {
    bool lux_valid;
    float lux;
    bool climate_valid;
    float temperature_c;
    float humidity_percent;
} rainmaker_environment_t;

typedef esp_err_t (*rainmaker_submit_target_fn)(uint8_t target, bool on,
                                                const char *source, uint32_t *command_id);
typedef esp_err_t (*rainmaker_get_command_result_fn)(uint32_t command_id,
                                                     rainmaker_command_result_t *result);
typedef esp_err_t (*rainmaker_get_light_state_fn)(uint8_t target,
                                                  rainmaker_light_state_t *state);
typedef void (*rainmaker_get_environment_fn)(rainmaker_environment_t *environment);

typedef struct {
    rainmaker_submit_target_fn submit_target;
    rainmaker_get_command_result_fn get_command_result;
    rainmaker_get_light_state_fn get_light_state;
    rainmaker_get_environment_fn get_environment;
    bool environment_available;
} rainmaker_control_api_t;

typedef struct {
    bool running;
    bool online;
    bool claimed;
    bool mapping_in_progress;
    int64_t last_success_uptime_ms;
    char node_id[RAINMAKER_NODE_ID_LENGTH];
    char last_error[96];
} rainmaker_extension_status_t;

esp_err_t rainmaker_extension_start(const rainmaker_control_api_t *control_api);
esp_err_t rainmaker_extension_stop(void);
esp_err_t rainmaker_extension_start_user_mapping(char *user_id, char *secret_key);
void rainmaker_extension_get_status(rainmaker_extension_status_t *status);
