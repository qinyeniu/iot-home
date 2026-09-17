/**
 * IoT-Home Zigbee Sensor Node v3.0
 *
 * - Joins Zigbee network (End Device, channel 26)
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
#include "aps/esp_zigbee_aps.h"
#include "driver/i2c_master.h"

// ==================== Configuration ====================

#define I2C_SDA_PIN             2
#define I2C_SCL_PIN             3
#define I2C_FREQ_HZ             100000
#define I2C_TIMEOUT_MS          100
#define I2C_SCAN_TIMEOUT_MS     50

#define AHT20_ADDR              0x38
#define BH1750_ADDR             0x23

#define AHT20_STATUS_BUSY       0x80
#define AHT20_STATUS_CALIBRATED 0x08
#define AHT20_DATA_LEN          7
#define AHT20_CRC_INIT          0xff
#define AHT20_CRC_POLYNOMIAL    0x31
#define AHT20_POWERUP_DELAY_MS  40
#define AHT20_MEASURE_DELAY_MS  80

#define AHT20_TEMP_MIN_C        (-40.0f)
#define AHT20_TEMP_MAX_C        85.0f
#define AHT20_HUM_MIN_PCT       0.0f
#define AHT20_HUM_MAX_PCT       100.0f
#define BH1750_LUX_MAX          60000.0f

#define SENSOR_INTERVAL_MS      10000   // debug period; deep-sleep phase will change this
#define REPORT_GAP_MS           200     // gap between the 3 ZCL reports
#define REPORT_SEND_ATTEMPTS    3       // immediate retries if the ZCL stack refuses a report
#define REPORT_RETRY_DELAY_MS   200
#define REPORT_APS_REPAIRS      2       // extra full-cache repairs after APS send failure
#define REPORT_STATS_MS         60000
#define REPORT_APS_RADIUS       10

// Minimal ZCL Report Attributes frame used by the raw APS path below.
#define ZCL_FRAME_REPORT_SERVER_TO_CLIENT  0x18
#define ZCL_CMD_REPORT_ATTRIBUTES          0x0a
#define ZCL_TYPE_UNSIGNED_16BIT            0x21
#define ZCL_TYPE_SIGNED_16BIT              0x29

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
static TaskHandle_t s_sensor_task_handle = NULL;
static uint8_t s_zcl_report_sequence = 0;

// Latest valid scaled ZCL values. They are used only to repair a report that
// was accepted but later failed at APS level; a fresh I2C failure does not
// publish stale readings as a new sample.
typedef struct {
    int16_t temp;
    uint16_t hum;
    uint16_t lux;
    uint8_t valid_flags;
} sensor_sample_t;

#define SAMPLE_FLAG_TEMP        (1u << 0)
#define SAMPLE_FLAG_HUM         (1u << 1)
#define SAMPLE_FLAG_LUX         (1u << 2)

typedef enum {
    SENSOR_READ_OK = 0,
    SENSOR_READ_BUS_ERROR,
    SENSOR_READ_INVALID,
} sensor_read_status_t;

static sensor_sample_t s_last_sample = {0};

static portMUX_TYPE s_report_state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_report_window_open = false;
static volatile uint8_t s_reports_in_flight = 0;
static volatile uint32_t s_aps_confirm_success = 0;
static volatile uint32_t s_aps_confirm_failure = 0;

static uint32_t s_i2c_th_failures = 0;
static uint32_t s_i2c_lux_failures = 0;
static uint32_t s_sensor_th_invalid = 0;
static uint32_t s_sensor_lux_invalid = 0;
static uint32_t s_th_fail_streak = 0;
static uint32_t s_lux_fail_streak = 0;
static uint32_t s_report_attempts = 0;
static uint32_t s_report_enqueued = 0;
static uint32_t s_report_enqueue_failures = 0;
static uint32_t s_report_items_failed = 0;
static uint32_t s_report_rounds = 0;
static uint32_t s_report_repairs = 0;

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_aht20_dev = NULL;
static i2c_master_dev_handle_t s_bh1750_dev = NULL;

// ==================== I2C bus / sensors ====================

static esp_err_t i2c_sensor_bus_init(void)
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
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: create bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t aht20_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &aht20_config, &s_aht20_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: add AHT20 failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t bh1750_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BH1750_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &bh1750_config, &s_bh1750_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: add BH1750 failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static void i2c_maybe_recover_bus(const char *sensor, esp_err_t err)
{
    if (err != ESP_ERR_TIMEOUT) {
        return;
    }

    ESP_LOGW(TAG, "I2C: %s transaction timed out; resetting bus", sensor);
    esp_err_t reset_err = i2c_master_bus_reset(s_i2c_bus);
    if (reset_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: bus reset failed after %s timeout: %s",
                 sensor, esp_err_to_name(reset_err));
    }
}

// ==================== AHT20 ====================

static bool aht20_init(void)
{
    uint8_t init_cmd[] = {0xBE, 0x08, 0x00};

    esp_err_t err = i2c_master_transmit(s_aht20_dev, init_cmd, sizeof(init_cmd),
                                        I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20: init command failed: %s", esp_err_to_name(err));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(AHT20_POWERUP_DELAY_MS));
    return true;
}

static bool aht20_crc_valid(const uint8_t data[static AHT20_DATA_LEN])
{
    uint8_t crc = AHT20_CRC_INIT;

    // The final byte is the CRC; it covers status plus five measurement bytes.
    for (size_t i = 0; i < AHT20_DATA_LEN - 1; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0 ? (uint8_t)((crc << 1) ^ AHT20_CRC_POLYNOMIAL)
                                     : (uint8_t)(crc << 1);
        }
    }

    return crc == data[AHT20_DATA_LEN - 1];
}

static sensor_read_status_t aht20_read(float *temp, float *hum)
{
    uint8_t trig_cmd[] = {0xAC, 0x33, 0x00};
    // Status + five measurement bytes + CRC-8.
    uint8_t data[AHT20_DATA_LEN];

    esp_err_t err = i2c_master_transmit(s_aht20_dev, trig_cmd, sizeof(trig_cmd),
                                        I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20: measurement trigger failed: %s", esp_err_to_name(err));
        i2c_maybe_recover_bus("AHT20 trigger", err);
        return SENSOR_READ_BUS_ERROR;
    }
    vTaskDelay(pdMS_TO_TICKS(AHT20_MEASURE_DELAY_MS));

    err = i2c_master_receive(s_aht20_dev, data, sizeof(data), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20: measurement receive failed: %s", esp_err_to_name(err));
        i2c_maybe_recover_bus("AHT20 receive", err);
        return SENSOR_READ_BUS_ERROR;
    }

    if ((data[0] & AHT20_STATUS_BUSY) != 0) {
        ESP_LOGW(TAG, "AHT20: measurement still busy, status=0x%02x", data[0]);
        return SENSOR_READ_INVALID;
    }
    if ((data[0] & AHT20_STATUS_CALIBRATED) == 0) {
        ESP_LOGW(TAG, "AHT20: not calibrated, status=0x%02x; reinitializing", data[0]);
        aht20_init();
        return SENSOR_READ_INVALID;
    }
    if (!aht20_crc_valid(data)) {
        ESP_LOGW(TAG,
                 "AHT20: bad measurement CRC, data=%02x %02x %02x %02x %02x %02x crc=%02x",
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
        return SENSOR_READ_INVALID;
    }

    uint32_t hum_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t temp_raw = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    float measured_temp = (float)temp_raw / 1048576.0f * 200.0f - 50.0f;
    float measured_hum = (float)hum_raw / 1048576.0f * 100.0f;

    if (measured_temp < AHT20_TEMP_MIN_C || measured_temp > AHT20_TEMP_MAX_C ||
        measured_hum < AHT20_HUM_MIN_PCT || measured_hum > AHT20_HUM_MAX_PCT) {
        ESP_LOGW(TAG,
                 "AHT20: rejecting out-of-range sample temp=%.2f hum=%.2f status=0x%02x",
                 measured_temp, measured_hum, data[0]);
        return SENSOR_READ_INVALID;
    }

    *temp = measured_temp;
    *hum = measured_hum;
    return SENSOR_READ_OK;
}

// ==================== BH1750 ====================

static sensor_read_status_t bh1750_read(float *lux)
{
    uint8_t cmd = 0x10;

    esp_err_t err = i2c_master_transmit(s_bh1750_dev, &cmd, sizeof(cmd), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BH1750: measurement trigger failed: %s", esp_err_to_name(err));
        i2c_maybe_recover_bus("BH1750 trigger", err);
        return SENSOR_READ_BUS_ERROR;
    }
    vTaskDelay(pdMS_TO_TICKS(180));

    uint8_t data[2];
    err = i2c_master_receive(s_bh1750_dev, data, sizeof(data), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BH1750: measurement receive failed: %s", esp_err_to_name(err));
        i2c_maybe_recover_bus("BH1750 receive", err);
        return SENSOR_READ_BUS_ERROR;
    }

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    float measured_lux = (float)raw / 1.2f;
    if (measured_lux < 0.0f || measured_lux > BH1750_LUX_MAX) {
        ESP_LOGW(TAG, "BH1750: rejecting out-of-range lux=%.1f raw=0x%04x",
                 measured_lux, raw);
        return SENSOR_READ_INVALID;
    }

    *lux = measured_lux;
    return SENSOR_READ_OK;
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

static bool aps_confirm_is_sensor_report(const esp_zb_apsde_data_confirm_t *confirm)
{
    return confirm->dst_addr_mode == ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT
        && confirm->dst_addr.addr_short == COORDINATOR_SHORT_ADDR
        && confirm->dst_endpoint == EP_SENSOR
        && confirm->src_endpoint == EP_SENSOR;
}

// Runs in Zigbee stack context. Keep it short: only record the APS result and
// wake the sensor task. Never send another Zigbee frame directly from here.
static void aps_data_confirm_cb(esp_zb_apsde_data_confirm_t confirm)
{
    bool app_report = false;

    if (aps_confirm_is_sensor_report(&confirm)) {
        taskENTER_CRITICAL(&s_report_state_lock);
        if (s_report_window_open && s_reports_in_flight > 0) {
            app_report = true;
            s_reports_in_flight--;
        }
        taskEXIT_CRITICAL(&s_report_state_lock);
    }

    if (!app_report) {
        return;
    }

    if (confirm.status == 0) {
        s_aps_confirm_success++;
        return;
    }

    s_aps_confirm_failure++;
    if (s_sensor_task_handle != NULL) {
        xTaskNotify(s_sensor_task_handle, 0, eNoAction);
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
    esp_zb_aps_data_confirm_handler_register(aps_data_confirm_cb);
    esp_zb_start(false);

    ESP_LOGI(TAG, "Zigbee stack started");

    while (1) {
        esp_zb_main_loop_iteration();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ==================== ZCL reporting ====================

static void report_window_reset(void)
{
    taskENTER_CRITICAL(&s_report_state_lock);
    s_report_window_open = true;
    s_reports_in_flight = 0;
    taskEXIT_CRITICAL(&s_report_state_lock);
}

static void report_window_close(void)
{
    taskENTER_CRITICAL(&s_report_state_lock);
    s_report_window_open = false;
    s_reports_in_flight = 0;
    taskEXIT_CRITICAL(&s_report_state_lock);
}

// Update the local ZCL attribute and send a Report Attributes frame through
// the raw APSDE-DATA service. The high-level ZCL helper does not produce the
// APS user-payload transmit confirmation used by the self-heal path; the raw
// request uses APS acknowledgement and invokes aps_data_confirm_cb().
static esp_err_t report_one_attribute(uint16_t cluster_id,
                                      uint16_t attr_id,
                                      const void *value)
{
    uint8_t attr_type = (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT)
                      ? ZCL_TYPE_SIGNED_16BIT : ZCL_TYPE_UNSIGNED_16BIT;
    uint8_t asdu[3 + 2 + 1 + 2] = {
        ZCL_FRAME_REPORT_SERVER_TO_CLIENT,
        ++s_zcl_report_sequence,
        ZCL_CMD_REPORT_ATTRIBUTES,
        (uint8_t)(attr_id & 0xff),
        (uint8_t)(attr_id >> 8),
        attr_type,
        0, 0,
    };
    memcpy(&asdu[6], value, sizeof(uint16_t));

    esp_zb_apsde_data_req_t req = {
        .dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .dst_addr.addr_short = COORDINATOR_SHORT_ADDR,
        .dst_endpoint = EP_SENSOR,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = cluster_id,
        .src_endpoint = EP_SENSOR,
        .asdu_length = sizeof(asdu),
        .asdu = asdu,
        .tx_options = ESP_ZB_APSDE_TX_OPT_SECURITY_ENABLED | ESP_ZB_APSDE_TX_OPT_ACK_TX,
        .use_alias = false,
        .alias_src_addr = 0,
        .alias_seq_num = 0,
        .radius = REPORT_APS_RADIUS,
    };
    esp_err_t err;

    esp_zb_lock_acquire(portMAX_DELAY);
    // Reserve the confirmation slot before the request so an unusually early APS
    // confirm cannot arrive before the in-flight counter has been incremented.
    taskENTER_CRITICAL(&s_report_state_lock);
    s_reports_in_flight++;
    taskEXIT_CRITICAL(&s_report_state_lock);

    esp_zb_zcl_set_attribute_val(EP_SENSOR, cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 attr_id, (void *)value, false);
    err = esp_zb_aps_data_request(&req);
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_report_state_lock);
        s_reports_in_flight--;
        taskEXIT_CRITICAL(&s_report_state_lock);
    }
    esp_zb_lock_release();

    return err;
}

static bool report_one_attribute_with_retry(uint16_t cluster_id,
                                            uint16_t attr_id,
                                            const void *value,
                                            const char *name)
{
    for (int attempt = 1; attempt <= REPORT_SEND_ATTEMPTS; ++attempt) {
        esp_err_t err;

        s_report_attempts++;
        err = report_one_attribute(cluster_id, attr_id, value);
        if (err == ESP_OK) {
            s_report_enqueued++;
            return true;
        }

        s_report_enqueue_failures++;
        ESP_LOGW(TAG, "REPORT_QUEUE: %s attempt %d/%d failed: %s",
                 name, attempt, REPORT_SEND_ATTEMPTS, esp_err_to_name(err));
        if (attempt < REPORT_SEND_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(REPORT_RETRY_DELAY_MS));
        }
    }

    s_report_items_failed++;
    return false;
}

static bool send_sensor_reports(uint8_t flags, const char *reason)
{
    bool all_ok = true;

    ESP_LOGI(TAG, "REPORT_SEND: reason=%s flags=0x%02x", reason, flags);

    if ((flags & SAMPLE_FLAG_TEMP) != 0) {
        all_ok &= report_one_attribute_with_retry(
            ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
            &s_last_sample.temp, "temperature");
        vTaskDelay(pdMS_TO_TICKS(REPORT_GAP_MS));
    }

    if ((flags & SAMPLE_FLAG_HUM) != 0) {
        all_ok &= report_one_attribute_with_retry(
            ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
            &s_last_sample.hum, "humidity");
        vTaskDelay(pdMS_TO_TICKS(REPORT_GAP_MS));
    }

    if ((flags & SAMPLE_FLAG_LUX) != 0) {
        all_ok &= report_one_attribute_with_retry(
            ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
            &s_last_sample.lux, "illuminance");
    }

    return all_ok;
}

static void log_report_stats(TickType_t now)
{
    static TickType_t last_stats_tick = 0;

    if (last_stats_tick == 0) {
        last_stats_tick = now;
        return;
    }
    if ((TickType_t)(now - last_stats_tick) < pdMS_TO_TICKS(REPORT_STATS_MS)) {
        return;
    }
    last_stats_tick = now;

    ESP_LOGI(TAG,
             "REPORT_STATS: rounds=%lu repairs=%lu i2c_th_fail=%lu i2c_lux_fail=%lu "
             "attempts=%lu queued=%lu queue_fail=%lu items_failed=%lu aps_ok=%lu aps_fail=%lu "
             "th_invalid=%lu lux_invalid=%lu th_streak=%lu lux_streak=%lu",
             (unsigned long)s_report_rounds,
             (unsigned long)s_report_repairs,
             (unsigned long)s_i2c_th_failures,
             (unsigned long)s_i2c_lux_failures,
             (unsigned long)s_report_attempts,
             (unsigned long)s_report_enqueued,
             (unsigned long)s_report_enqueue_failures,
             (unsigned long)s_report_items_failed,
             (unsigned long)s_aps_confirm_success,
             (unsigned long)s_aps_confirm_failure,
             (unsigned long)s_sensor_th_invalid,
             (unsigned long)s_sensor_lux_invalid,
             (unsigned long)s_th_fail_streak,
             (unsigned long)s_lux_fail_streak);
}

static void sensor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));

    bool aht20_ready = aht20_init();
    ESP_LOGI(TAG, "AHT20 initial setup: %s", aht20_ready ? "OK" : "failed; will retry");

    while (1) {
        if (!s_zigbee_connected) {
            ESP_LOGI(TAG, "Waiting for Zigbee network...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Drop a notification left by a report from an earlier reporting window.
        xTaskNotifyWait(0, 0, NULL, 0);
        report_window_close();

        float temp = 0, hum = 0, lux = 0;
        sensor_read_status_t th_status = SENSOR_READ_BUS_ERROR;
        if (aht20_ready) {
            th_status = aht20_read(&temp, &hum);
            if (th_status == SENSOR_READ_BUS_ERROR) {
                // Reinitialize once on the next round after NACK/timeout. A
                // timeout has already attempted an I2C bus reset inside read.
                aht20_ready = false;
            }
        } else {
            aht20_ready = aht20_init();
            if (aht20_ready) {
                ESP_LOGI(TAG, "AHT20 setup recovered");
            }
        }
        sensor_read_status_t lux_status = bh1750_read(&lux);
        bool have_temp_hum = th_status == SENSOR_READ_OK;
        bool have_lux = lux_status == SENSOR_READ_OK;
        uint8_t report_flags = 0;

        if (have_temp_hum) {
            if (s_th_fail_streak != 0) {
                ESP_LOGI(TAG, "AHT20: readings recovered after %lu failed round(s)",
                         (unsigned long)s_th_fail_streak);
            }
            s_th_fail_streak = 0;
            // ZCL scaling: temp int16 in 0.01 C; humidity uint16 in 0.01 %.
            s_last_sample.temp = (int16_t)(temp * 100.0f);
            s_last_sample.hum = (uint16_t)(hum * 100.0f);
            s_last_sample.valid_flags |= SAMPLE_FLAG_TEMP | SAMPLE_FLAG_HUM;
            report_flags |= SAMPLE_FLAG_TEMP | SAMPLE_FLAG_HUM;
        } else if (th_status == SENSOR_READ_INVALID) {
            s_sensor_th_invalid++;
            s_th_fail_streak++;
        } else {
            s_i2c_th_failures++;
            s_th_fail_streak++;
            ESP_LOGW(TAG, "AHT20: temperature/humidity bus read unavailable");
        }

        if (have_lux) {
            if (s_lux_fail_streak != 0) {
                ESP_LOGI(TAG, "BH1750: readings recovered after %lu failed round(s)",
                         (unsigned long)s_lux_fail_streak);
            }
            s_lux_fail_streak = 0;
            // Illuminance uses the Zigbee log scale: 10000 * log10(lux + 1).
            float log_val = lux > 0.0f ? 10000.0f * log10f(lux + 1.0f) : 0.0f;
            s_last_sample.lux = (uint16_t)log_val;
            s_last_sample.valid_flags |= SAMPLE_FLAG_LUX;
            report_flags |= SAMPLE_FLAG_LUX;
        } else if (lux_status == SENSOR_READ_INVALID) {
            s_sensor_lux_invalid++;
            s_lux_fail_streak++;
        } else {
            s_i2c_lux_failures++;
            s_lux_fail_streak++;
            ESP_LOGW(TAG, "BH1750: illuminance bus read unavailable");
        }

        ESP_LOGI(TAG,
                 "DATA: {\"temp\":%.1f,\"hum\":%.1f,\"lux\":%.1f,\"addr\":\"0x%04x\","
                 "\"th_ok\":%s,\"lux_ok\":%s}",
                 have_temp_hum ? temp : 0.0f,
                 have_temp_hum ? hum : 0.0f,
                 have_lux ? lux : 0.0f,
                 s_short_addr,
                 have_temp_hum ? "true" : "false",
                 have_lux ? "true" : "false");

        s_report_rounds++;
        report_window_reset();
        bool queued = send_sensor_reports(report_flags, "sample");
        log_report_stats(xTaskGetTickCount());

        if (!queued) {
            // The next sample is taken quickly. If I2C is healthy it supersedes
            // the failed values; if I2C also fails, APS repair can still use cache.
            report_window_close();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SENSOR_INTERVAL_MS);
        uint32_t repairs_this_round = 0;

        while (repairs_this_round < REPORT_APS_REPAIRS) {
            TickType_t now = xTaskGetTickCount();
            TickType_t remaining = (TickType_t)(deadline - now);
            if ((int32_t)remaining <= 0) {
                break;
            }

            if (xTaskNotifyWait(0, 0, NULL, remaining) != pdTRUE) {
                break;  // Normal 10-second reporting period elapsed.
            }

            repairs_this_round++;
            s_report_repairs++;
            ESP_LOGW(TAG,
                     "REPORT_APS: send failure #%lu confirmed; repair %lu/%u",
                     (unsigned long)s_aps_confirm_failure,
                     (unsigned long)repairs_this_round,
                     REPORT_APS_REPAIRS);

            vTaskDelay(pdMS_TO_TICKS(1000 * repairs_this_round));
            report_window_reset();
            if (!send_sensor_reports(s_last_sample.valid_flags, "aps-repair")) {
                ESP_LOGE(TAG, "REPORT_APS: repair reports could not be queued");
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }
        }

        report_window_close();
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

    ESP_ERROR_CHECK(i2c_sensor_bus_init());
    ESP_LOGI(TAG, "I2C init OK (SDA=%d SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);

    // Temporary wiring diagnostic: scan bus for AHT20 (0x38) / BH1750 (0x23)
    for (uint8_t a = 1; a < 127; a++) {
        if (i2c_master_probe(s_i2c_bus, a, I2C_SCAN_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGW(TAG, "I2C SCAN: found 0x%02x", a);
        }
    }
    ESP_LOGW(TAG, "I2C SCAN: expect AHT20=0x38 BH1750=0x23");

    // Create the sensor task first so its notification handle exists before
    // the Zigbee task registers the APS confirm callback.
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, &s_sensor_task_handle);
    xTaskCreate(zigbee_task, "zigbee", 16384, NULL, 5, NULL);

    ESP_LOGI(TAG, "Zigbee sensor node ready!");
}
