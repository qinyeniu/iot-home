/*
 * IoT-Home battery sensor SED skeleton.
 *
 * This is an independent experimental firmware: it does not modify the
 * mains-powered combined sensor/relay node. It joins as a Zigbee Sleepy End
 * Device on channel 26 and keeps the measurement endpoint ready while the
 * application work is added incrementally.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "nwk/esp_zigbee_nwk.h"

#define APP_ENDPOINT              10
#define STARTUP_TX_POWER_DBM      (-10)
#define ED_KEEP_ALIVE_MS          60000U
#define SLEEP_THRESHOLD_MS        500U
#define ZIGBEE_CHANNEL_MASK       (1L << 26)

static const char *TAG = "batt-sed";

/*
 * Avoid tight RF scanning loops when the coordinator is unavailable or the BDB
 * state machine is still busy. The first delayed retry is 5 seconds; after
 * repeated failures the device stays at a 60-second cadence.
 */
static const uint32_t s_commission_backoff_ms[] = {
    5000,
    30000,
    60000,
};
static uint8_t s_commission_backoff_index = 0;

static void commissioning_timer_cb(uint8_t mode_mask);

static void commissioning_reset_backoff(void)
{
    s_commission_backoff_index = 0;
}

static void commissioning_schedule(uint8_t mode_mask, const char *reason)
{
    uint8_t index = s_commission_backoff_index;
    uint32_t delay_ms = s_commission_backoff_ms[index];

    if (index < (sizeof(s_commission_backoff_ms) /
                 sizeof(s_commission_backoff_ms[0])) - 1) {
        s_commission_backoff_index++;
    }

    ESP_LOGI(TAG, "Commissioning %s: mode=0x%x retry in %lu ms",
             reason, (unsigned)mode_mask, (unsigned long)delay_ms);
    esp_zb_scheduler_alarm_cancel(commissioning_timer_cb, mode_mask);
    esp_zb_scheduler_alarm(commissioning_timer_cb, mode_mask, delay_ms);
}

static void commissioning_timer_cb(uint8_t mode_mask)
{
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(mode_mask);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Commissioning request rejected: %s", esp_err_to_name(err));
        commissioning_schedule(mode_mask, "request busy");
    }
}

static esp_err_t app_core_action_handler(
    esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    (void)callback_id;
    (void)message;
    return ESP_OK;
}

static esp_zb_ep_list_t *create_sensor_endpoint(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x03,  /* Battery */
    };
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = 0x8000,
        .min_value = -4000,
        .max_value = 8500,
    };
    esp_zb_humidity_meas_cluster_cfg_t humidity_cfg = {
        .measured_value = 0xFFFF,
        .min_value = 0,
        .max_value = 10000,
    };
    esp_zb_illuminance_meas_cluster_cfg_t illuminance_cfg = {
        .measured_value = 0xFFFF,
        .min_value = 0,
        .max_value = 0xFFFD,
    };

    esp_zb_cluster_list_add_basic_cluster(
        clusters, esp_zb_basic_cluster_create(&basic_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(
        clusters, esp_zb_identify_cluster_create(&identify_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_temperature_meas_cluster(
        clusters, esp_zb_temperature_meas_cluster_create(&temp_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_humidity_meas_cluster(
        clusters, esp_zb_humidity_meas_cluster_create(&humidity_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_illuminance_meas_cluster(
        clusters, esp_zb_illuminance_meas_cluster_create(&illuminance_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t endpoint_cfg = {
        .endpoint = APP_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, clusters, endpoint_cfg);
    return ep_list;
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_signal = signal_struct->p_app_signal;
    esp_err_t signal_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t signal_type = *p_signal;

    switch (signal_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP: {
        esp_err_t err = esp_zb_bdb_start_top_level_commissioning(
            ESP_ZB_BDB_MODE_INITIALIZATION);

        ESP_LOGI(TAG, "Initialize Zigbee stack");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Stack initialization rejected: %s",
                     esp_err_to_name(err));
            commissioning_schedule(ESP_ZB_BDB_MODE_INITIALIZATION,
                                   "initialization");
        }
        break;
    }

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (signal_status == ESP_OK) {
            commissioning_reset_backoff();
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory-new device; start network steering");
                commissioning_schedule(ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                       "factory-new");
            } else {
                ESP_LOGI(TAG, "Rebooted and rejoined previous network");
            }
        } else {
            ESP_LOGW(TAG, "Startup failed: %s; retry initialization",
                     esp_err_to_name(signal_status));
            commissioning_schedule(ESP_ZB_BDB_MODE_INITIALIZATION,
                                   "startup failed");
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (signal_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;

            commissioning_reset_backoff();
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG,
                     "Joined network: PAN=0x%04x extPAN=%02x:%02x:%02x:%02x:"
                     "%02x:%02x:%02x:%02x channel=%d short=0x%04x",
                     esp_zb_get_pan_id(),
                     extended_pan_id[7], extended_pan_id[6],
                     extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2],
                     extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_current_channel(),
                     esp_zb_get_short_address());
        } else {
            ESP_LOGW(TAG, "Network steering failed: %s",
                     esp_err_to_name(signal_status));
            commissioning_schedule(ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                   "steering failed");
        }
        break;

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        esp_zb_sleep_now();
        break;

    default:
        ESP_LOGD(TAG, "Zigbee signal=0x%x status=%s",
                 (unsigned)signal_type, esp_err_to_name(signal_status));
        break;
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;
    esp_zb_cfg_t zed_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_256MIN,
            .keep_alive = ED_KEEP_ALIVE_MS,
        },
    };

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Starting battery SED");

    esp_zb_sleep_enable(true);
    ESP_ERROR_CHECK(esp_zb_sleep_set_threshold(SLEEP_THRESHOLD_MS));
    esp_zb_init(&zed_cfg);

    esp_zb_set_tx_power(STARTUP_TX_POWER_DBM);
    ESP_LOGI(TAG, "Startup TX power target=%d dBm", STARTUP_TX_POWER_DBM);

    /* A SED must not keep the receiver on while idle. */
    esp_zb_set_rx_on_when_idle(false);
    ESP_LOGI(TAG, "RX-on-when-idle=%d", esp_zb_get_rx_on_when_idle());

    ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(ZIGBEE_CHANNEL_MASK));
    ESP_ERROR_CHECK(esp_zb_device_register(create_sensor_endpoint()));
    esp_zb_core_action_handler_register(app_core_action_handler);
    ESP_ERROR_CHECK(esp_zb_start(false));

    esp_zb_stack_main_loop();
}

static esp_err_t power_management_init(void)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    return esp_pm_configure(&pm_config);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(power_management_init());

    ESP_LOGI(TAG, "Battery sensor SED skeleton starting");
    xTaskCreate(zigbee_task, "zigbee", 6144, NULL, 5, NULL);
}
