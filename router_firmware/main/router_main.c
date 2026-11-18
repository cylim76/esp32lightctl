#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_zigbee.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "ezbee/nwk.h"

#define ROUTER_CHANNEL                 15
#define ROUTER_CHANNEL_MASK            (1UL << ROUTER_CHANNEL)
#define ROUTER_ENDPOINT                1
#define ROUTER_MAX_CHILDREN            16
#define ROUTER_TX_POWER_DBM            20
#define ROUTER_STEERING_RETRY_MS       10000
#define ROUTER_HEALTH_LOG_MS           30000
#define ROUTER_ZIGBEE_PARTITION        "zigbee_nvs"

/* Zigbee character strings carry their byte length in the first octet. */
static uint8_t s_manufacturer_name[] = {13, 'e','s','p','3','2','l','i','g','h','t','c','t','l'};
static uint8_t s_model_identifier[] = {
    22, 'E','S','P','3','2','-','C','6','-','Z','i','g','b','e','e','-','R','o','u','t','e','r'
};

static const char *TAG = "zigbee_router";
static bool s_joined;
static uint32_t s_join_attempt;

static void log_network(const char *prefix)
{
    esp_zb_ieee_addr_t ext_pan = {0};
    esp_zb_get_extended_pan_id(ext_pan);
    ESP_LOGI(TAG,
             "%s joined=%s channel=%u PAN=0x%04x short=0x%04x extPAN="
             "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x free_heap=%" PRIu32,
             prefix, s_joined ? "yes" : "no", esp_zb_get_current_channel(),
             esp_zb_get_pan_id(), esp_zb_get_short_address(),
             ext_pan[7], ext_pan[6], ext_pan[5], ext_pan[4],
             ext_pan[3], ext_pan[2], ext_pan[1], ext_pan[0],
             esp_get_free_heap_size());
}

static void commissioning_retry(uint8_t mode)
{
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Commissioning start failed: %s", esp_err_to_name(err));
        esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_retry, mode,
                               ROUTER_STEERING_RETRY_MS);
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal)
{
    uint32_t *raw_signal = signal->p_app_signal;
    esp_zb_app_signal_type_t type = *raw_signal;
    esp_err_t status = signal->esp_err_status;

    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee Router stack");
        commissioning_retry(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status != ESP_OK) {
            ESP_LOGE(TAG, "Router initialization failed: %s", esp_err_to_name(status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_retry,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        } else if (esp_zb_bdb_is_factory_new()) {
            s_joined = false;
            ESP_LOGI(TAG, "Factory-new router; scanning channel %u for an open network",
                     ROUTER_CHANNEL);
            commissioning_retry(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            s_joined = true;
            s_join_attempt = 0;
            log_network("Restored Zigbee Router network");
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            s_joined = true;
            s_join_attempt = 0;
            log_network("Joined Zigbee network");
        } else {
            s_joined = false;
            s_join_attempt++;
            ESP_LOGW(TAG,
                     "Join attempt %" PRIu32 " failed: %s; retry in %u seconds "
                     "(gateway Permit Join must be open)",
                     s_join_attempt, esp_err_to_name(status),
                     ROUTER_STEERING_RETRY_MS / 1000);
            esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_retry,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                   ROUTER_STEERING_RETRY_MS);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION:
        s_joined = false;
        ESP_LOGW(TAG, "Router left the network; waiting to rejoin on channel %u",
                 ROUTER_CHANNEL);
        esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_retry,
                               ESP_ZB_BDB_MODE_NETWORK_STEERING,
                               ROUTER_STEERING_RETRY_MS);
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *device =
            esp_zb_app_signal_get_params(raw_signal);
        ESP_LOGI(TAG, "Child/device announce short=0x%04x ieee="
                      "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 device->device_short_addr,
                 device->ieee_addr[7], device->ieee_addr[6],
                 device->ieee_addr[5], device->ieee_addr[4],
                 device->ieee_addr[3], device->ieee_addr[2],
                 device->ieee_addr[1], device->ieee_addr[0]);
        break;
    }

    default:
        ESP_LOGD(TAG, "Signal %s status=%s",
                 esp_zb_zdo_signal_to_string(type), esp_err_to_name(status));
        break;
    }
}

static void health_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ROUTER_HEALTH_LOG_MS));
        if (s_joined) {
            esp_zb_lock_acquire(portMAX_DELAY);
            log_network("Router health");
            esp_zb_lock_release();
        } else {
            ESP_LOGW(TAG, "Router not joined; waiting for gateway Permit Join on channel %u",
                     ROUTER_CHANNEL);
        }
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;
    const esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config = {.max_children = ROUTER_MAX_CHILDREN},
        },
        .platform_config = {
            .storage_partition_name = ROUTER_ZIGBEE_PARTITION,
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ezb_nwk_set_ed_timeout(EZB_NWK_ED_TIMEOUT_16384MIN);
    ezb_nwk_set_tx_power(ROUTER_TX_POWER_DBM);
    esp_zb_set_primary_network_channel_set(ROUTER_CHANNEL_MASK);
    esp_zb_set_secondary_network_channel_set(0);

    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_identify_cluster_cfg_t identify_cfg = {.identify_time = 0};
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer_name));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model_identifier));
    esp_zb_cluster_list_add_basic_cluster(
        clusters, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(
        clusters, esp_zb_identify_cluster_create(&identify_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *endpoints = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t endpoint_cfg = {
        .endpoint = ROUTER_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(endpoints, clusters, endpoint_cfg));
    ESP_ERROR_CHECK(esp_zb_device_register(endpoints));

    ESP_LOGI(TAG,
             "Starting dedicated router: channel=%u max_children=%u tx_power=%ddBm "
             "Wi-Fi=disabled",
             ROUTER_CHANNEL, ROUTER_MAX_CHILDREN, ROUTER_TX_POWER_DBM);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "esp32lightctl Zigbee Router firmware %s",
             esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Primary NVS init failed: %s; no partition was erased",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_flash_init_partition(ROUTER_ZIGBEE_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee NVS init failed: %s; no partition was erased",
                 esp_err_to_name(err));
        return;
    }

    if (xTaskCreate(health_task, "router_health", 3072, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(zigbee_task, "zigbee_router", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create router tasks");
        return;
    }
}
