/**
 * IoT-Home Zigbee Sensor Node v3.0
 *
 * - Joins Zigbee network (End Device, channel 25)
 * - Reads AHT20 (temp/hum) + BH1750 (lux) over I2C
 * - Reports sensor values via standard ZCL attribute reports to coordinator
 *   (EP10, HA profile, Temperature/Humidity/Illuminance Measurement clusters)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"
#include "nwk/esp_zigbee_nwk.h"
#include "driver/i2c.h"

// ==================== Configuration ====================

#define I2C_SDA_PIN             2
#define I2C_SCL_PIN             3
#define I2C_FREQ_HZ             100000

#define AHT20_ADDR              0x38
#define BH1750_ADDR             0x23

#define SENSOR_INTERVAL_MS      10000   // debug period; deep-sleep phase will change this
#define REPORT_GAP_MS           200     // gap between the 3 ZCL reports

#define EP_SENSOR               10      // endpoint on both node and gateway
#define COORDINATOR_SHORT_ADDR  0x0000

#define INSTALLCODE_POLICY      false
#define ESP_ZB_CHANNEL_MASK     (1l << 26)

// Zigbee End Device config
#define ESP_ZB_ZED_CONFIG()                         \
    {                                               \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,       \
        .install_code_policy = INSTALLCODE_POLICY,  \
        .nwk_cfg.zed_cfg = {                        \
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN, \
            .keep_alive = 3000,                     \
        },                                          \
    }

// ==================== Globals ====================

static const char *TAG = "zb-sensor";

static volatile bool s_zigbee_connected = false;
static uint16_t s_short_addr = 0;

// ==================== AHT20 ====================

static bool aht20_read(float *temp, float *hum)
{
    uint8_t init_cmd[] = {0xBE, 0x08, 0x00};
    uint8_t trig_cmd[] = {0xAC, 0x33, 0x00};
    uint8_t data[6];

    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (AHT20_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, init_cmd, 3, true);
    i2c_master_stop(h);
    if (i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100)) != ESP_OK) {
        i2c_cmd_link_delete(h);
        return false;
    }
    i2c_cmd_link_delete(h);
    vTaskDelay(pdMS_TO_TICKS(10));

    h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (AHT20_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, trig_cmd, 3, true);
    i2c_master_stop(h);
    if (i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100)) != ESP_OK) {
        i2c_cmd_link_delete(h);
        return false;
    }
    i2c_cmd_link_delete(h);
    vTaskDelay(pdMS_TO_TICKS(80));

    h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (AHT20_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(h, data, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(h);
    if (i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100)) != ESP_OK) {
        i2c_cmd_link_delete(h);
        return false;
    }
    i2c_cmd_link_delete(h);

    uint32_t hum_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t temp_raw = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *temp = (float)temp_raw / 1048576.0f * 200.0f - 50.0f;
    *hum = (float)hum_raw / 1048576.0f * 100.0f;

    return true;
}

// ==================== BH1750 ====================

static bool bh1750_read(float *lux)
{
    uint8_t cmd = 0x10;

    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (BH1750_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, cmd, true);
    i2c_master_stop(h);
    if (i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100)) != ESP_OK) {
        i2c_cmd_link_delete(h);
        return false;
    }
    i2c_cmd_link_delete(h);
    vTaskDelay(pdMS_TO_TICKS(180));

    uint8_t data[2];
    h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (BH1750_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(h, data, 2, I2C_MASTER_LAST_NACK);
    i2c_master_stop(h);
    if (i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100)) != ESP_OK) {
        i2c_cmd_link_delete(h);
        return false;
    }
    i2c_cmd_link_delete(h);

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *lux = (float)raw / 1.2f;

    return true;
}

// ==================== Zigbee endpoint / clusters ====================

static esp_zb_ep_list_t *create_sensor_ep(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();

    // Mandatory server clusters for an HA device
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x03,  // battery
    };
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_basic_cluster(clusters,
        esp_zb_basic_cluster_create(&basic_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(clusters,
        esp_zb_identify_cluster_create(&identify_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Measurement server clusters; 0x8000/0xFFFF = invalid until first sample
    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = 0x8000, .min_value = -4000, .max_value = 8500,
    };
    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = 0xFFFF, .min_value = 0, .max_value = 10000,
    };
    esp_zb_illuminance_meas_cluster_cfg_t lux_cfg = {
        .measured_value = 0xFFFF, .min_value = 0, .max_value = 0xFFFD,
    };
    esp_zb_cluster_list_add_temperature_meas_cluster(clusters,
        esp_zb_temperature_meas_cluster_create(&temp_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_humidity_meas_cluster(clusters,
        esp_zb_humidity_meas_cluster_create(&hum_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_illuminance_meas_cluster(clusters,
        esp_zb_illuminance_meas_cluster_create(&lux_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = EP_SENSOR,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
    return ep_list;
}

// ==================== Zigbee signals ====================

static void bdb_commissioning_cb(uint8_t mode_mask)
{
    esp_zb_bdb_start_top_level_commissioning(mode_mask);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee: Initialize stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee: Device started");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Zigbee: Start steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Zigbee: Rebooted, connected");
                s_zigbee_connected = true;
                s_short_addr = esp_zb_get_short_address();
                ESP_LOGI(TAG, "Zigbee: Short addr=0x%04x", s_short_addr);
            }
        } else {
            ESP_LOGW(TAG, "Zigbee: Startup/rejoin failed (0x%x), retry steering", (unsigned)err_status);
            // A previously commissioned ZED first attempts a silent rejoin.
            // If that parent/rejoin attempt fails, explicitly run network
            // steering instead of waiting forever for another signal.
            if (sig_type == ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT) {
                esp_zb_scheduler_alarm(bdb_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee: Connected to network!");
            s_zigbee_connected = true;
            s_short_addr = esp_zb_get_short_address();
            ESP_LOGI(TAG, "Zigbee: Short addr=0x%04x", s_short_addr);
            esp_zb_scheduler_alarm_cancel(bdb_commissioning_cb,
                                          ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Zigbee: Steering failed (0x%x), retry in 5s", (unsigned)err_status);
            esp_zb_scheduler_alarm(bdb_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 5000);
        }
        break;
    default:
        ESP_LOGI(TAG, "Zigbee signal: %d", sig_type);
        break;
    }
}

static void zigbee_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Starting Zigbee end device...");

    esp_zb_cfg_t zb_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_cfg);
    // Temporary diagnostic: keep this ZED receiver on instead of sleepy polling.
    esp_zb_set_rx_on_when_idle(true);
    ESP_LOGI(TAG, "RX-on-when-idle=%d", esp_zb_get_rx_on_when_idle());
    esp_zb_set_primary_network_channel_set(ESP_ZB_CHANNEL_MASK);
    esp_zb_device_register(create_sensor_ep());
    esp_zb_start(false);

    ESP_LOGI(TAG, "Zigbee stack started");

    while (1) {
        esp_zb_main_loop_iteration();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ==================== ZCL reporting ====================

// Update local attribute value then unicast a Report Attributes command to
// the coordinator. Must be called while holding the Zigbee stack lock.
static void report_one_attribute(uint16_t cluster_id, uint16_t attr_id, const void *value)
{
    esp_zb_zcl_set_attribute_val(EP_SENSOR, cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 attr_id, (void *)value, false);

    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = COORDINATOR_SHORT_ADDR,
            .dst_endpoint = EP_SENSOR,
            .src_endpoint = EP_SENSOR,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .attributeID = attr_id,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
    };
    esp_zb_zcl_report_attr_cmd_req(&cmd);
}

static void sensor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        if (!s_zigbee_connected) {
            ESP_LOGI(TAG, "Waiting for Zigbee network...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        float temp = 0, hum = 0, lux = 0;
        bool have_temp = aht20_read(&temp, &hum);
        bool have_lux = bh1750_read(&lux);

        ESP_LOGI(TAG, "DATA: {\"temp\":%.1f,\"hum\":%.1f,\"lux\":%.1f,\"addr\":\"0x%04x\"}",
                 have_temp ? temp : 0.0f, have_temp ? hum : 0.0f,
                 have_lux ? lux : 0.0f, s_short_addr);

        // ZCL scaling: temp int16 in 0.01 C; hum uint16 in 0.01 %;
        // illuminance uint16 on Zigbee log scale: 10000 * log10(lux + 1)
        esp_zb_lock_acquire(portMAX_DELAY);

        if (have_temp) {
            int16_t temp_raw = (int16_t)(temp * 100.0f);
            uint16_t hum_raw = (uint16_t)(hum * 100.0f);
            report_one_attribute(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                                 ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &temp_raw);
            vTaskDelay(pdMS_TO_TICKS(REPORT_GAP_MS));
            report_one_attribute(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                                 ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &hum_raw);
            vTaskDelay(pdMS_TO_TICKS(REPORT_GAP_MS));
        }
        if (have_lux) {
            float log_val = lux > 0.0f ? 10000.0f * log10f(lux + 1.0f) : 0.0f;
            uint16_t lux_raw = (uint16_t)log_val;
            report_one_attribute(ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
                                 ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID, &lux_raw);
        }

        esp_zb_lock_release();

        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// ==================== Main ====================

void app_main(void)
{
    ESP_LOGI(TAG, "=============================");
    ESP_LOGI(TAG, "Zigbee Sensor Node v3.0 (ZCL)");
    ESP_LOGI(TAG, "=============================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_NUM_0, &i2c_cfg);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    ESP_LOGI(TAG, "I2C init OK (SDA=%d SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);

    // Temporary wiring diagnostic: scan bus for AHT20 (0x38) / BH1750 (0x23)
    for (uint8_t a = 1; a < 127; a++) {
        i2c_cmd_handle_t hh = i2c_cmd_link_create();
        i2c_master_start(hh);
        i2c_master_write_byte(hh, (a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(hh);
        esp_err_t r = i2c_master_cmd_begin(I2C_NUM_0, hh, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(hh);
        if (r == ESP_OK) ESP_LOGW(TAG, "I2C SCAN: found 0x%02x", a);
    }
    ESP_LOGW(TAG, "I2C SCAN: expect AHT20=0x38 BH1750=0x23");

    xTaskCreate(zigbee_task, "zigbee", 16384, NULL, 5, NULL);
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Zigbee sensor node ready!");
}
