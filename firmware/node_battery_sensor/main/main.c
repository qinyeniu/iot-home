/*
 * IoT-Home battery sensor SED.
 *
 * Independent experimental firmware: it does not modify the mains-powered
 * combined sensor/relay node. It joins as a Zigbee Sleepy End Device on
 * channel 26, periodically reads AHT20 + BH1750, and reports measurements to
 * the coordinator with the same standard ZCL frame format as node_zigbee.
 */

#include <math.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"
#include "driver/i2c_master.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "nwk/esp_zigbee_nwk.h"
#include "aps/esp_zigbee_aps.h"

#define APP_ENDPOINT              10
#define STARTUP_TX_POWER_DBM      CONFIG_BATT_STARTUP_TX_POWER_DBM
#define ED_KEEP_ALIVE_MS          CONFIG_BATT_KEEP_ALIVE_MS
#define SLEEP_THRESHOLD_MS        500U
#define ZIGBEE_CHANNEL_MASK       (1L << 26)

#define I2C_SDA_PIN               2
#define I2C_SCL_PIN               3
#define I2C_FREQ_HZ               100000
#define I2C_TIMEOUT_MS            100
#define I2C_BUS_REINIT_FAILURES   3

#define AHT20_ADDR                0x38
#define BH1750_ADDR               0x23
#define AHT20_STATUS_BUSY         0x80
#define AHT20_STATUS_CALIBRATED   0x08
#define AHT20_DATA_LEN            7
#define AHT20_CRC_INIT            0xff
#define AHT20_CRC_POLYNOMIAL      0x31
#define AHT20_POWERUP_DELAY_MS    40
#define AHT20_MEASURE_DELAY_MS    80

#define AHT20_TEMP_MIN_C          (-40.0f)
#define AHT20_TEMP_MAX_C          85.0f
#define AHT20_HUM_MIN_PCT         0.0f
#define AHT20_HUM_MAX_PCT         100.0f
#define BH1750_LUX_MAX            60000.0f
#define ZCL_ILLUMINANCE_MAX       0xFFFD

#define SENSOR_INTERVAL_MS        (CONFIG_BATT_SAMPLE_INTERVAL_S * 1000)
#define REPORT_GAP_MS             CONFIG_BATT_REPORT_GAP_MS
#define REPORT_LOCAL_ATTEMPTS     CONFIG_BATT_REPORT_LOCAL_ATTEMPTS
#define REPORT_RETRY_DELAY_MS     200
#define REPORT_APS_RADIUS         10

#define ZCL_FRAME_REPORT_SERVER_TO_CLIENT 0x18
#define ZCL_CMD_REPORT_ATTRIBUTES        0x0a
#define ZCL_TYPE_UNSIGNED_16BIT          0x21
#define ZCL_TYPE_SIGNED_16BIT            0x29

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
static uint8_t s_zcl_report_sequence = 0;

static volatile bool s_joined = false;
static volatile uint16_t s_short_address = 0;
static uint32_t s_bus_error_streak = 0;
static uint32_t s_aps_ack_ok = 0;
static uint32_t s_aps_ack_fail = 0;

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_aht20_dev = NULL;
static i2c_master_dev_handle_t s_bh1750_dev = NULL;

typedef struct {
    int16_t temperature;
    uint16_t humidity;
    uint16_t illuminance;
    uint8_t valid_flags;
} sensor_sample_t;

#define SAMPLE_FLAG_TEMP  (1u << 0)
#define SAMPLE_FLAG_HUM   (1u << 1)
#define SAMPLE_FLAG_LUX   (1u << 2)

typedef enum {
    SENSOR_READ_OK = 0,
    SENSOR_READ_BUS_ERROR,
    SENSOR_READ_INVALID,
} sensor_read_status_t;

static sensor_sample_t s_latest_sample = {0};

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
        ESP_LOGW(TAG, "Commissioning request rejected: %s",
                 esp_err_to_name(err));
        commissioning_schedule(mode_mask, "request busy");
    }
}

static void mark_joined(const char *reason)
{
    commissioning_reset_backoff();
    s_short_address = esp_zb_get_short_address();
    s_joined = true;
    ESP_LOGI(TAG, "Zigbee joined (%s), short=0x%04x", reason, s_short_address);
}

static void mark_offline(const char *reason)
{
    s_joined = false;
    ESP_LOGW(TAG, "Zigbee offline: %s", reason);
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
        .max_value = ZCL_ILLUMINANCE_MAX,
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
            if (esp_zb_bdb_is_factory_new()) {
                mark_offline("factory-new before steering");
                ESP_LOGI(TAG, "Factory-new device; start network steering");
                commissioning_schedule(ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                       "factory-new");
            } else {
                mark_joined("device reboot");
            }
        } else {
            uint8_t recovery_mode =
                (signal_type == ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT)
                ? ESP_ZB_BDB_MODE_NETWORK_STEERING
                : ESP_ZB_BDB_MODE_INITIALIZATION;

            mark_offline("startup failed");
            ESP_LOGW(TAG, "Startup failed: %s", esp_err_to_name(signal_status));
            commissioning_schedule(recovery_mode, "startup failed");
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (signal_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;

            esp_zb_get_extended_pan_id(extended_pan_id);
            mark_joined("network steering");
            ESP_LOGI(TAG,
                     "Joined network: PAN=0x%04x extPAN=%02x:%02x:%02x:%02x:"
                     "%02x:%02x:%02x:%02x channel=%d",
                     esp_zb_get_pan_id(),
                     extended_pan_id[7], extended_pan_id[6],
                     extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2],
                     extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_current_channel());
        } else {
            mark_offline("network steering failed");
            ESP_LOGW(TAG, "Network steering failed: %s",
                     esp_err_to_name(signal_status));
            commissioning_schedule(ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                   "steering failed");
        }
        break;

    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        mark_offline("no active links");
        commissioning_schedule(ESP_ZB_BDB_MODE_NETWORK_STEERING,
                               "no active links");
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

static void i2c_bus_deinit(void)
{
    esp_err_t err;

    if (s_aht20_dev != NULL) {
        err = i2c_master_bus_rm_device(s_aht20_dev);
        if (err == ESP_OK) {
            s_aht20_dev = NULL;
        } else {
            ESP_LOGE(TAG, "I2C: remove AHT20 failed: %s",
                     esp_err_to_name(err));
        }
    }

    if (s_bh1750_dev != NULL) {
        err = i2c_master_bus_rm_device(s_bh1750_dev);
        if (err == ESP_OK) {
            s_bh1750_dev = NULL;
        } else {
            ESP_LOGE(TAG, "I2C: remove BH1750 failed: %s",
                     esp_err_to_name(err));
        }
    }

    if (s_i2c_bus != NULL) {
        err = i2c_del_master_bus(s_i2c_bus);
        if (err == ESP_OK) {
            s_i2c_bus = NULL;
        } else {
            ESP_LOGE(TAG, "I2C: delete bus failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t aht20_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    i2c_device_config_t bh1750_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BH1750_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    esp_err_t err;

    i2c_bus_deinit();

    err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        i2c_bus_deinit();
        return err;
    }

    err = i2c_master_bus_add_device(s_i2c_bus, &aht20_config, &s_aht20_dev);
    if (err != ESP_OK) {
        i2c_bus_deinit();
        return err;
    }

    err = i2c_master_bus_add_device(s_i2c_bus, &bh1750_config, &s_bh1750_dev);
    if (err != ESP_OK) {
        i2c_bus_deinit();
        return err;
    }

    return ESP_OK;
}

static bool i2c_bus_ready(void)
{
    return s_i2c_bus != NULL && s_aht20_dev != NULL && s_bh1750_dev != NULL;
}

static esp_err_t i2c_bus_rebuild(const char *reason)
{
    ESP_LOGW(TAG, "I2C: rebuild bus because %s", reason);
    return i2c_bus_init();
}

static bool aht20_init(void)
{
    uint8_t init_cmd[] = {0xBE, 0x08, 0x00};

    if (s_aht20_dev == NULL) {
        return false;
    }

    esp_err_t err = i2c_master_transmit(s_aht20_dev, init_cmd,
                                        sizeof(init_cmd), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 init failed: %s", esp_err_to_name(err));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(AHT20_POWERUP_DELAY_MS));
    return true;
}

static bool aht20_crc_valid(const uint8_t data[static AHT20_DATA_LEN])
{
    uint8_t crc = AHT20_CRC_INIT;

    for (size_t i = 0; i < AHT20_DATA_LEN - 1; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = ((crc & 0x80U) != 0)
                ? (uint8_t)((crc << 1) ^ AHT20_CRC_POLYNOMIAL)
                : (uint8_t)(crc << 1);
        }
    }

    return crc == data[AHT20_DATA_LEN - 1];
}

static sensor_read_status_t aht20_read(float *temperature, float *humidity)
{
    uint8_t trigger_cmd[] = {0xAC, 0x33, 0x00};
    uint8_t data[AHT20_DATA_LEN];

    if (s_aht20_dev == NULL) {
        return SENSOR_READ_BUS_ERROR;
    }

    esp_err_t err = i2c_master_transmit(s_aht20_dev, trigger_cmd,
                                        sizeof(trigger_cmd), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 trigger failed: %s", esp_err_to_name(err));
        return SENSOR_READ_BUS_ERROR;
    }

    vTaskDelay(pdMS_TO_TICKS(AHT20_MEASURE_DELAY_MS));
    err = i2c_master_receive(s_aht20_dev, data, sizeof(data), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 receive failed: %s", esp_err_to_name(err));
        return SENSOR_READ_BUS_ERROR;
    }

    if ((data[0] & AHT20_STATUS_BUSY) != 0) {
        return SENSOR_READ_INVALID;
    }
    if ((data[0] & AHT20_STATUS_CALIBRATED) == 0) {
        (void)aht20_init();
        return SENSOR_READ_INVALID;
    }
    if (!aht20_crc_valid(data)) {
        return SENSOR_READ_INVALID;
    }

    uint32_t humidity_raw =
        ((uint32_t)data[1] << 12) |
        ((uint32_t)data[2] << 4) |
        (data[3] >> 4);
    uint32_t temperature_raw =
        (((uint32_t)data[3] & 0x0F) << 16) |
        ((uint32_t)data[4] << 8) |
        data[5];

    float measured_temperature =
        (float)temperature_raw / 1048576.0f * 200.0f - 50.0f;
    float measured_humidity =
        (float)humidity_raw / 1048576.0f * 100.0f;

    if (measured_temperature < AHT20_TEMP_MIN_C ||
        measured_temperature > AHT20_TEMP_MAX_C ||
        measured_humidity < AHT20_HUM_MIN_PCT ||
        measured_humidity > AHT20_HUM_MAX_PCT) {
        return SENSOR_READ_INVALID;
    }

    *temperature = measured_temperature;
    *humidity = measured_humidity;
    return SENSOR_READ_OK;
}

static sensor_read_status_t bh1750_read(float *lux)
{
    /* One-shot high-resolution mode: BH1750 powers down after conversion. */
    uint8_t command = 0x20;
    uint8_t data[2];

    if (s_bh1750_dev == NULL) {
        return SENSOR_READ_BUS_ERROR;
    }

    esp_err_t err = i2c_master_transmit(s_bh1750_dev, &command,
                                        sizeof(command), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 trigger failed: %s", esp_err_to_name(err));
        return SENSOR_READ_BUS_ERROR;
    }

    vTaskDelay(pdMS_TO_TICKS(180));
    err = i2c_master_receive(s_bh1750_dev, data, sizeof(data), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 receive failed: %s", esp_err_to_name(err));
        return SENSOR_READ_BUS_ERROR;
    }

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    float measured_lux = (float)raw / 1.2f;

    if (measured_lux > BH1750_LUX_MAX) {
        return SENSOR_READ_INVALID;
    }

    *lux = measured_lux;
    return SENSOR_READ_OK;
}

static bool measurement_confirm_is_ours(
    const esp_zb_apsde_data_confirm_t *confirm)
{
    return confirm != NULL &&
           confirm->asdu != NULL &&
           confirm->asdu_length >= (3 + 2 + 1 + 2) &&
           confirm->asdu[0] == ZCL_FRAME_REPORT_SERVER_TO_CLIENT &&
           confirm->asdu[2] == ZCL_CMD_REPORT_ATTRIBUTES &&
           confirm->dst_addr_mode == ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT &&
           confirm->dst_addr.addr_short == 0x0000 &&
           confirm->dst_endpoint == APP_ENDPOINT &&
           confirm->src_endpoint == APP_ENDPOINT;
}

static void aps_data_confirm_cb(esp_zb_apsde_data_confirm_t confirm)
{
    if (!measurement_confirm_is_ours(&confirm)) {
        return;
    }

    if (confirm.status == 0) {
        s_aps_ack_ok++;
        ESP_LOGI(TAG, "APS ack ok seq=%u total=%lu",
                 (unsigned)confirm.asdu[1], (unsigned long)s_aps_ack_ok);
    } else {
        s_aps_ack_fail++;
        ESP_LOGW(TAG, "APS ack failed seq=%u status=%u total=%lu; wait next sample",
                 (unsigned)confirm.asdu[1], (unsigned)confirm.status,
                 (unsigned long)s_aps_ack_fail);
    }
}

static esp_err_t report_one_attribute(uint16_t cluster_id,
                                      uint16_t attribute_id,
                                      const void *value)
{
    uint8_t attribute_type =
        (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT)
        ? ZCL_TYPE_SIGNED_16BIT
        : ZCL_TYPE_UNSIGNED_16BIT;
    uint8_t asdu[3 + 2 + 1 + 2] = {
        ZCL_FRAME_REPORT_SERVER_TO_CLIENT,
        ++s_zcl_report_sequence,
        ZCL_CMD_REPORT_ATTRIBUTES,
        (uint8_t)(attribute_id & 0xff),
        (uint8_t)(attribute_id >> 8),
        attribute_type,
        0, 0,
    };
    esp_zb_apsde_data_req_t request = {
        .dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .dst_addr.addr_short = 0x0000,
        .dst_endpoint = APP_ENDPOINT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = cluster_id,
        .src_endpoint = APP_ENDPOINT,
        .asdu_length = sizeof(asdu),
        .asdu = asdu,
        .tx_options = ESP_ZB_APSDE_TX_OPT_SECURITY_ENABLED |
                       ESP_ZB_APSDE_TX_OPT_ACK_TX,
        .use_alias = false,
        .alias_src_addr = 0,
        .alias_seq_num = 0,
        .radius = REPORT_APS_RADIUS,
    };
    esp_err_t err;

    memcpy(&asdu[6], value, sizeof(uint16_t));

    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)esp_zb_zcl_set_attribute_val(
        APP_ENDPOINT, cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        attribute_id, (void *)value, false);
    err = esp_zb_aps_data_request(&request);
    esp_zb_lock_release();

    return err;
}

static bool report_with_local_retry(uint16_t cluster_id,
                                    uint16_t attribute_id,
                                    const void *value,
                                    const char *name)
{
    for (int attempt = 1; attempt <= REPORT_LOCAL_ATTEMPTS; ++attempt) {
        esp_err_t err = report_one_attribute(cluster_id, attribute_id, value);
        if (err == ESP_OK) {
            return true;
        }

        ESP_LOGW(TAG, "Report %s locally rejected, attempt %d/%d: %s",
                 name, attempt, REPORT_LOCAL_ATTEMPTS, esp_err_to_name(err));
        if (attempt < REPORT_LOCAL_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(REPORT_RETRY_DELAY_MS));
        }
    }

    return false;
}

static void mark_measurements_invalid(uint8_t invalid_flags)
{
    static const int16_t temperature_invalid = 0x8000;
    static const uint16_t humidity_invalid = 0xFFFF;
    static const uint16_t illuminance_invalid = 0xFFFF;

    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        ESP_LOGW(TAG, "Cannot mark measurements invalid: Zigbee lock busy");
        return;
    }

    if ((invalid_flags & SAMPLE_FLAG_TEMP) != 0) {
        (void)esp_zb_zcl_set_attribute_val(
            APP_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
            (void *)&temperature_invalid, false);
    }
    if ((invalid_flags & SAMPLE_FLAG_HUM) != 0) {
        (void)esp_zb_zcl_set_attribute_val(
            APP_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
            (void *)&humidity_invalid, false);
    }
    if ((invalid_flags & SAMPLE_FLAG_LUX) != 0) {
        (void)esp_zb_zcl_set_attribute_val(
            APP_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
            (void *)&illuminance_invalid, false);
    }

    esp_zb_lock_release();
}
static bool send_sample_reports(const sensor_sample_t *sample,
                                    uint8_t report_flags)
{
    bool all_queued = true;

    if ((report_flags & SAMPLE_FLAG_TEMP) != 0) {
        all_queued &= report_with_local_retry(
            ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
            &sample->temperature, "temperature");
        vTaskDelay(pdMS_TO_TICKS(REPORT_GAP_MS));
    }

    if ((report_flags & SAMPLE_FLAG_HUM) != 0) {
        all_queued &= report_with_local_retry(
            ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
            &sample->humidity, "humidity");
        vTaskDelay(pdMS_TO_TICKS(REPORT_GAP_MS));
    }

    if ((report_flags & SAMPLE_FLAG_LUX) != 0) {
        all_queued &= report_with_local_retry(
            ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
            &sample->illuminance, "illuminance");
    }

    return all_queued;
}

static void sensor_task(void *arg)
{
    bool aht20_ready = false;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));

    for (;;) {
        if (!s_joined) {
            if (i2c_bus_ready()) {
                i2c_bus_deinit();
                aht20_ready = false;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!i2c_bus_ready()) {
            if (i2c_bus_rebuild("bus unavailable") != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }

        if (!aht20_ready) {
            aht20_ready = aht20_init();
        }

        float temperature = 0.0f;
        float humidity = 0.0f;
        float lux = 0.0f;
        sensor_read_status_t temperature_status =
            aht20_ready ? aht20_read(&temperature, &humidity)
                        : SENSOR_READ_BUS_ERROR;
        sensor_read_status_t lux_status = bh1750_read(&lux);
        uint8_t report_flags = 0;

        if (temperature_status == SENSOR_READ_OK) {
            s_bus_error_streak = 0;
            s_latest_sample.temperature = (int16_t)(temperature * 100.0f);
            s_latest_sample.humidity = (uint16_t)(humidity * 100.0f);
            s_latest_sample.valid_flags |= SAMPLE_FLAG_TEMP | SAMPLE_FLAG_HUM;
            report_flags |= SAMPLE_FLAG_TEMP | SAMPLE_FLAG_HUM;
        } else {
            mark_measurements_invalid(SAMPLE_FLAG_TEMP | SAMPLE_FLAG_HUM);
            s_latest_sample.valid_flags &=
                (uint8_t)~(SAMPLE_FLAG_TEMP | SAMPLE_FLAG_HUM);
            if (temperature_status == SENSOR_READ_BUS_ERROR) {
                aht20_ready = false;
                s_bus_error_streak++;
            }
        }

        if (lux_status == SENSOR_READ_OK) {
            s_bus_error_streak = 0;
            float log_value = 10000.0f * log10f(lux + 1.0f);

            if (log_value < 0.0f) {
                log_value = 0.0f;
            }
            if (log_value > (float)ZCL_ILLUMINANCE_MAX) {
                log_value = (float)ZCL_ILLUMINANCE_MAX;
            }

            s_latest_sample.illuminance = (uint16_t)log_value;
            s_latest_sample.valid_flags |= SAMPLE_FLAG_LUX;
            report_flags |= SAMPLE_FLAG_LUX;
        } else {
            mark_measurements_invalid(SAMPLE_FLAG_LUX);
            s_latest_sample.valid_flags &= (uint8_t)~SAMPLE_FLAG_LUX;
            if (lux_status == SENSOR_READ_BUS_ERROR) {
                s_bus_error_streak++;
            }
        }

        ESP_LOGI(TAG,
                 "DATA: {\"temp\":%.1f,\"hum\":%.1f,\"lux\":%.1f,\"addr\":\"0x%04x\","
                 "\"th_ok\":%s,\"lux_ok\":%s}",
                 temperature_status == SENSOR_READ_OK ? temperature : 0.0f,
                 temperature_status == SENSOR_READ_OK ? humidity : 0.0f,
                 lux_status == SENSOR_READ_OK ? lux : 0.0f,
                 s_short_address,
                 temperature_status == SENSOR_READ_OK ? "true" : "false",
                 lux_status == SENSOR_READ_OK ? "true" : "false");

        if (report_flags != 0) {
            bool queued = send_sample_reports(&s_latest_sample, report_flags);
            ESP_LOGI(TAG, "Sample reports queued=%s, aps_ok=%lu aps_fail=%lu",
                     queued ? "true" : "partial",
                     (unsigned long)s_aps_ack_ok,
                     (unsigned long)s_aps_ack_fail);
        }

        if (s_bus_error_streak >= I2C_BUS_REINIT_FAILURES) {
            s_bus_error_streak = 0;
            aht20_ready = false;
            if (i2c_bus_rebuild("repeated I2C failures") != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        /* Release the I2C controller between samples; sensors use low-power
         * states and BH1750 was already started in one-shot mode. */
        i2c_bus_deinit();
        aht20_ready = false;
        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
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
    esp_zb_aps_data_confirm_handler_register(aps_data_confirm_cb);
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

    ESP_LOGI(TAG, "Battery sensor SED starting");

    ESP_LOGI(TAG, "I2C is powered on demand after Zigbee joins");

    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    xTaskCreate(zigbee_task, "zigbee", 6144, NULL, 5, NULL);
}
