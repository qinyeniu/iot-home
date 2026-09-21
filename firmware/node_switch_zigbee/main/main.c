/**
 * IoT-Home Switch Node (Zigbee End Device)
 *
 * Receives standard ZCL On/Off commands (cluster 0x0006) relayed by the
 * gateway from the cloud, and drives a relay.
 *
 * Hardware:
 *  - ESP32-C6 SuperMini
 *  - 3.3V low-level-trigger relay on IN=GPIO4 (LOW energizes / HIGH releases)
 *
 * The node joins the existing Zigbee network; NVRAM is preserved.
 */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#define SWITCH_CHANNEL_MASK (1l << 26)
#define SWITCH_ENDPOINT     10
#define RELAY_GPIO          GPIO_NUM_4

static const char *TAG = "switch-node";

static volatile bool s_join_settled = false;
static volatile bool s_relay_on = false;

static bool short_address_is_usable(uint16_t short_addr)
{
    return short_addr != 0x0000 &&
           short_addr != 0xfffc &&
           short_addr != 0xfffd &&
           short_addr != 0xfffe;
}

// Relay is low-level triggered: drive the pin LOW to energize it.
static void relay_set(bool on)
{
    s_relay_on = on;
    gpio_set_level(RELAY_GPIO, on ? 0 : 1);
    ESP_LOGI(TAG, "RELAY %s", on ? "ON" : "OFF");
}

static void bdb_steering_cb(uint8_t mode_mask)
{
    if (s_join_settled) {
        ESP_LOGW(TAG, "ignore stale steering request after association");
        return;
    }
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

// HA library turns an incoming On/Off command into an attribute-set action.
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *data)
{
    if (callback_id != ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return ESP_OK;
    }
    const esp_zb_zcl_set_attr_value_message_t *msg = data;
    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        msg->attribute.data.size == 1 &&
        msg->attribute.data.value != NULL) {
        bool on = (*(const uint8_t *)msg->attribute.data.value) != 0;
        relay_set(on);
    }
    return ESP_OK;
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "initialize stack");
        ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION));
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "factory-new: scan and join on channel 26");
                ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING));
            } else {
                ESP_LOGI(TAG, "rebooted with prior Zigbee state");
            }
        } else {
            ESP_LOGE(TAG, "device start failed: %s", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            s_join_settled = true;
            esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)bdb_steering_cb,
                                         ESP_ZB_BDB_MODE_NETWORK_STEERING);
            esp_zb_ieee_addr_t ext_pan;
            esp_zb_get_extended_pan_id(ext_pan);
            ESP_LOGI(TAG,
                     "JOINED PAN=0x%04hx channel=%d short=0x%04hx xpan=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                     esp_zb_get_short_address(),
                     ext_pan[7], ext_pan[6], ext_pan[5], ext_pan[4],
                     ext_pan[3], ext_pan[2], ext_pan[1], ext_pan[0]);
        } else {
            esp_zb_bdb_commissioning_status_t bdb_status =
                esp_zb_get_bdb_commissioning_status();
            uint16_t short_addr = esp_zb_get_short_address();
            ESP_LOGW(TAG,
                     "STEERING_RESULT status=%s bdb=%d short=0x%04hx",
                     esp_err_to_name(err_status), (int)bdb_status, short_addr);
            if ((bdb_status == ESP_ZB_BDB_STATUS_DEV_ANNCE_SEND_FAILURE ||
                 bdb_status == ESP_ZB_BDB_STATUS_ON_A_NETWORK) &&
                short_address_is_usable(short_addr)) {
                s_join_settled = true;
                esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)bdb_steering_cb,
                                              ESP_ZB_BDB_MODE_NETWORK_STEERING);
                ESP_LOGW(TAG, "provisional association accepted bdb=%d", (int)bdb_status);
            } else {
                ESP_LOGW(TAG, "retry steering in 5 seconds");
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_steering_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 5000);
            }
        }
        break;
    default:
        ESP_LOGI(TAG, "signal=%s(0x%x) status=%s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static void zigbee_task(void *arg)
{
    esp_zb_cfg_t cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&cfg);
    // NVRAM is intentionally preserved across reboots.

    esp_zb_on_off_light_cfg_t ep_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *ep = esp_zb_on_off_light_ep_create(SWITCH_ENDPOINT, &ep_cfg);
    ESP_ERROR_CHECK(ep != NULL ? ESP_OK : ESP_FAIL);
    esp_zb_device_register(ep);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_rx_on_when_idle(true);

    ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(SWITCH_CHANNEL_MASK));
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // Configure the relay pin before the radio starts and force it HIGH so
    // the relay stays released (off) during and just after power-up.
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(RELAY_GPIO, 1);

    ESP_LOGI(TAG, "IoT switch node starting; relay OFF");
    xTaskCreate(zigbee_task, "zb_switch", 4096, NULL, 5, NULL);
}
