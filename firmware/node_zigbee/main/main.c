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
#include <stddef.h>
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
#define I2C_BUS_REINIT_FAILURES 3

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
#define ZIGBEE_LINK_RESTEER_DELAY_MS  1000
#define ZIGBEE_STEERING_FAIL_RETRY_MS 5000
#define ZIGBEE_BDB_BUSY_RETRY_MS   60000
#define ZB_UNAVAILABLE_VERIFY_DELAY_MS 10000
#define LINK_PROBE_MAX_ATTEMPTS    4
#define LINK_PROBE_RETRY_MS        1500
#define ZB_REJOIN_CANDIDATE_VERIFY_DELAY_MS 30000
#define ZB_REJOIN_CANDIDATE_FIRST_PROBE_MS 2000
#define ZB_REJOIN_CANDIDATE_PROBE_MAX_ATTEMPTS 10
#define ZB_REJOIN_CANDIDATE_PROBE_RETRY_MS 2500
#define LINK_PROBE_RETIRED_COUNT 16
#define LINK_PROBE_RETIRED_TICKS pdMS_TO_TICKS(60000)
#define ZIGBEE_RECENT_STEERING_ATTEMPT_MS 90000
#define ZIGBEE_STALE_COMMISSIONING_WINDOW_MS 2000

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
static volatile bool s_network_steering_pending = false;
static volatile bool s_link_resteer_forced = false;
static uint32_t s_bdb_busy_since_ms = 0;
static uint32_t s_last_steering_attempt_ms = 0;
static uint32_t s_rejoin_candidate_ms = 0;
static uint16_t s_short_addr = 0;
static TaskHandle_t s_sensor_task_handle = NULL;

#define SENSOR_NOTIFY_APS_FAIL_BIT  (1u << 0)
#define SENSOR_NOTIFY_LINK_PROBE_BIT (1u << 1)

// A single ZDO unavailable signal can refer to one failed NWK/MAC/APS packet.
// It does not by itself prove the parent relationship is gone. The signal
// handler sends one independent APS probe and only resteers if that probe does
// not receive a successful APS confirmation before the verification alarm.
typedef enum {
    LINK_PROBE_IDLE = 0,
    LINK_PROBE_ACTIVE,
} link_probe_state_t;

static volatile uint32_t s_device_unavailable_total = 0;
static volatile uint32_t s_device_unavailable_recovered = 0;
static volatile uint32_t s_device_unavailable_verify_resteers = 0;
static volatile uint32_t s_candidate_rejoin_total = 0;
static volatile uint32_t s_candidate_rejoin_verified = 0;
static volatile uint32_t s_link_probe_requests = 0;
static volatile uint32_t s_link_probe_sent = 0;
static volatile uint32_t s_link_probe_aps_failures = 0;
static portMUX_TYPE s_link_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile link_probe_state_t s_link_probe_state = LINK_PROBE_IDLE;
static volatile uint8_t s_link_probe_expected_sequence = 0;
static volatile uint8_t s_link_probe_attempts = 0;
static volatile bool s_link_probe_in_flight = false;
static volatile bool s_link_probe_candidate = false;
static uint32_t s_link_probe_timeout_ms = ZB_UNAVAILABLE_VERIFY_DELAY_MS;
static uint32_t s_link_probe_retry_ms = LINK_PROBE_RETRY_MS;
static uint8_t s_link_probe_max_attempts = LINK_PROBE_MAX_ATTEMPTS;
static uint8_t s_zcl_report_sequence = 0;

typedef struct {
    bool used;
    uint8_t sequence;
    TickType_t expires_at;
} link_probe_retired_t;

static link_probe_retired_t s_link_probe_retired[LINK_PROBE_RETIRED_COUNT];

static void link_probe_reset_locked(void)
{
    s_link_probe_state = LINK_PROBE_IDLE;
    s_link_probe_expected_sequence = 0;
    s_link_probe_attempts = 0;
    s_link_probe_in_flight = false;
    s_link_probe_candidate = false;
}

static void link_probe_retire_locked(uint8_t sequence, TickType_t now)
{
    int slot = -1;
    int oldest = 0;

    for (int i = 0; i < LINK_PROBE_RETIRED_COUNT; ++i) {
        if (!s_link_probe_retired[i].used ||
            (int32_t)(now - s_link_probe_retired[i].expires_at) >= 0) {
            slot = i;
            break;
        }
        if ((int32_t)(s_link_probe_retired[i].expires_at -
                      s_link_probe_retired[oldest].expires_at) < 0) {
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
    }

    s_link_probe_retired[slot].used = true;
    s_link_probe_retired[slot].sequence = sequence;
    s_link_probe_retired[slot].expires_at =
        (TickType_t)(now + LINK_PROBE_RETIRED_TICKS);
}

static bool link_probe_take_retired_locked(uint8_t sequence, TickType_t now)
{
    for (int i = 0; i < LINK_PROBE_RETIRED_COUNT; ++i) {
        if (!s_link_probe_retired[i].used) {
            continue;
        }
        if ((int32_t)(now - s_link_probe_retired[i].expires_at) >= 0) {
            s_link_probe_retired[i].used = false;
            continue;
        }
        if (s_link_probe_retired[i].sequence == sequence) {
            s_link_probe_retired[i].used = false;
            return true;
        }
    }
    return false;
}

static void link_probe_cancel_active_preserve_retired(void)
{
    TickType_t now = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_link_probe_lock);
    if (s_link_probe_state == LINK_PROBE_ACTIVE) {
        if (s_link_probe_in_flight) {
            link_probe_retire_locked(s_link_probe_expected_sequence, now);
        }
        link_probe_reset_locked();
    }
    taskEXIT_CRITICAL(&s_link_probe_lock);
}


// Both running-link probes and provisional-rejoin probes own the radio window.
// Keeping normal three-attribute batches out prevents the storm observed after
// a rejoin (dozens of failed APS frames before the parent path has settled).
static bool link_probe_blocks_reports(void)
{
    bool blocked;

    taskENTER_CRITICAL(&s_link_probe_lock);
    blocked = s_link_probe_state == LINK_PROBE_ACTIVE;
    taskEXIT_CRITICAL(&s_link_probe_lock);
    return blocked;
}

static bool link_probe_claim_send(uint8_t *sequence)
{
    bool claimed = false;

    taskENTER_CRITICAL(&s_link_probe_lock);
    if (s_link_probe_state == LINK_PROBE_ACTIVE && s_zigbee_connected) {
        *sequence = ++s_zcl_report_sequence;
        s_link_probe_expected_sequence = *sequence;
        s_link_probe_attempts++;
        s_link_probe_in_flight = true;
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_link_probe_lock);
    return claimed;
}

static bool zcl_report_sequence_allocate(uint8_t *sequence)
{
    bool allocated = false;

    taskENTER_CRITICAL(&s_link_probe_lock);
    if (s_link_probe_state == LINK_PROBE_IDLE) {
        *sequence = ++s_zcl_report_sequence;
        allocated = true;
    }
    taskEXIT_CRITICAL(&s_link_probe_lock);
    return allocated;
}

static void link_probe_send_timer_cb(uint8_t unused);

// A local submission rejection means no APS transaction was started. Do not
// spend one of the APS retry attempts; release the in-flight slot and re-arm
// the same probe from the serialized ZBOSS scheduler callback.
static void link_probe_handle_local_submit_failure(uint8_t sequence,
                                                   const char *reason)
{
    bool retry = false;

    taskENTER_CRITICAL(&s_link_probe_lock);
    if (s_link_probe_state == LINK_PROBE_ACTIVE &&
        s_link_probe_expected_sequence == sequence) {
        if (s_link_probe_attempts > 0) {
            s_link_probe_attempts--;
        }
        s_link_probe_in_flight = false;
        retry = true;
    }
    taskEXIT_CRITICAL(&s_link_probe_lock);

    if (retry) {
        ESP_LOGW(TAG,
                 "Zigbee: %s probe seq=%u locally; retry in %lu ms",
                 reason, (unsigned)sequence,
                 (unsigned long)s_link_probe_retry_ms);
        esp_zb_scheduler_alarm_cancel(link_probe_send_timer_cb, 0);
        esp_zb_scheduler_alarm(link_probe_send_timer_cb, 0,
                               s_link_probe_retry_ms);
    }
}

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
static uint32_t s_i2c_bus_resets = 0;
static uint32_t s_i2c_bus_reinits = 0;
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

static void i2c_sensor_bus_deinit(void)
{
    esp_err_t err;

    if (s_aht20_dev != NULL) {
        err = i2c_master_bus_rm_device(s_aht20_dev);
        if (err == ESP_OK) {
            s_aht20_dev = NULL;
        } else {
            ESP_LOGE(TAG, "I2C: remove AHT20 device failed: %s", esp_err_to_name(err));
        }
    }

    if (s_bh1750_dev != NULL) {
        err = i2c_master_bus_rm_device(s_bh1750_dev);
        if (err == ESP_OK) {
            s_bh1750_dev = NULL;
        } else {
            ESP_LOGE(TAG, "I2C: remove BH1750 device failed: %s", esp_err_to_name(err));
        }
    }

    if (s_i2c_bus != NULL) {
        err = i2c_del_master_bus(s_i2c_bus);
        if (err == ESP_OK) {
            s_i2c_bus = NULL;
        } else {
            ESP_LOGE(TAG, "I2C: delete bus failed: %s", esp_err_to_name(err));
        }
    }
}

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

    // Rebuilding is only called from the sensor task, but make repeated calls
    // safe and ensure a partially initialized previous attempt is released.
    i2c_sensor_bus_deinit();

    err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: create bus failed: %s", esp_err_to_name(err));
        i2c_sensor_bus_deinit();
        return err;
    }

    err = i2c_master_bus_add_device(s_i2c_bus, &aht20_config, &s_aht20_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: add AHT20 failed: %s", esp_err_to_name(err));
        i2c_sensor_bus_deinit();
        return err;
    }

    err = i2c_master_bus_add_device(s_i2c_bus, &bh1750_config, &s_bh1750_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: add BH1750 failed: %s", esp_err_to_name(err));
        i2c_sensor_bus_deinit();
        return err;
    }

    return ESP_OK;
}

static bool i2c_sensor_bus_ready(void)
{
    return s_i2c_bus != NULL && s_aht20_dev != NULL && s_bh1750_dev != NULL;
}

static esp_err_t i2c_sensor_bus_rebuild(const char *reason)
{
    s_i2c_bus_reinits++;
    ESP_LOGW(TAG, "I2C: rebuilding bus because %s (#%lu)",
             reason, (unsigned long)s_i2c_bus_reinits);

    esp_err_t err = i2c_sensor_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: rebuild after %s failed: %s",
                 reason, esp_err_to_name(err));
    }

    return err;
}

static void i2c_maybe_recover_bus(const char *sensor, esp_err_t err)
{
    if (err != ESP_ERR_TIMEOUT || s_i2c_bus == NULL) {
        return;
    }

    s_i2c_bus_resets++;
    ESP_LOGW(TAG, "I2C: %s transaction timed out; resetting bus (#%lu)",
             sensor, (unsigned long)s_i2c_bus_resets);
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

    if (s_aht20_dev == NULL) {
        ESP_LOGW(TAG, "AHT20: init skipped because I2C device is not ready");
        return false;
    }

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

    if (s_aht20_dev == NULL) {
        ESP_LOGW(TAG, "AHT20: measurement skipped because I2C device is not ready");
        return SENSOR_READ_BUS_ERROR;
    }
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

    if (s_bh1750_dev == NULL) {
        ESP_LOGW(TAG, "BH1750: measurement skipped because I2C device is not ready");
        return SENSOR_READ_BUS_ERROR;
    }

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

static void network_steering_timer_cb(uint8_t mode_mask);
static void schedule_network_steering(uint32_t delay_ms);
static void report_window_close(void);
static void zigbee_mark_connected(const char *reason);
static void zigbee_link_lost(esp_zb_app_signal_type_t signal, esp_err_t status);
static void device_unavailable_verify_timer_cb(uint8_t unused);
static void link_probe_send_timer_cb(uint8_t unused);

// Minimal ABI-compatible view of the private ZBOSS device-unavailable payload.
// This node has only one peer (the coordinator), so the target address lets us
// ignore unrelated stack signals without including an internal ZBOSS header.
typedef struct {
    uint8_t long_addr[8];
    uint16_t short_addr;
} node_zb_device_unavailable_params_t;

_Static_assert(sizeof(node_zb_device_unavailable_params_t) == 10,
               "device-unavailable payload ABI size mismatch");
_Static_assert(offsetof(node_zb_device_unavailable_params_t, short_addr) == 8,
               "device-unavailable short address offset mismatch");

static bool link_probe_build_frame(uint8_t asdu[3 + 2 + 1 + 2],
                                   uint16_t *cluster_id,
                                   uint16_t *attr_id,
                                   bool allow_invalid_candidate_value)
{
    uint16_t value;
    uint8_t attr_type;

    if ((s_last_sample.valid_flags & SAMPLE_FLAG_TEMP) != 0) {
        *cluster_id = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        *attr_id = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID;
        attr_type = ZCL_TYPE_SIGNED_16BIT;
        value = s_last_sample.temp;
    } else if ((s_last_sample.valid_flags & SAMPLE_FLAG_HUM) != 0) {
        *cluster_id = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
        *attr_id = ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID;
        attr_type = ZCL_TYPE_UNSIGNED_16BIT;
        value = s_last_sample.hum;
    } else if ((s_last_sample.valid_flags & SAMPLE_FLAG_LUX) != 0) {
        *cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT;
        *attr_id = ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID;
        attr_type = ZCL_TYPE_UNSIGNED_16BIT;
        value = s_last_sample.lux;
    } else if (allow_invalid_candidate_value) {
        // A fresh reboot has not sampled the sensors yet. Verify the parent path
        // with the temperature measurement cluster using its standard invalid
        // value (0x8000). The gateway explicitly discards this value, so it only
        // exercises APS/ZCL delivery and is never published as telemetry.
        *cluster_id = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        *attr_id = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID;
        attr_type = ZCL_TYPE_SIGNED_16BIT;
        value = 0x8000;
    } else {
        // Do not use a Basic-cluster fallback: the gateway application does not
        // process that report as a sensor measurement.
        return false;
    }

    asdu[0] = ZCL_FRAME_REPORT_SERVER_TO_CLIENT;
    // Sequence is assigned by the caller after the active probe generation has
    // been claimed; the remaining bytes form one ZCL Report Attributes record.
    asdu[2] = ZCL_CMD_REPORT_ATTRIBUTES;
    asdu[3] = (uint8_t)(*attr_id & 0xff);
    asdu[4] = (uint8_t)(*attr_id >> 8);
    asdu[5] = attr_type;
    memcpy(&asdu[6], &value, sizeof(value));
    return true;
}

// Runs in the serialized ZBOSS scheduler. This probe is deliberately separate
// from the normal sensor reporting window so a late ordinary APS confirm can
// neither consume a probe result nor trigger the normal full-cache repair path.
static void link_probe_send_timer_cb(uint8_t unused)
{
    uint8_t asdu[3 + 2 + 1 + 2] = {0};
    uint16_t cluster_id = 0;
    uint16_t attr_id = 0;
    uint8_t sequence;
    esp_err_t err;

    (void)unused;

    if (!link_probe_claim_send(&sequence)) {
        return;
    }

    if (!link_probe_build_frame(asdu, &cluster_id, &attr_id,
                               s_link_probe_candidate)) {
        ESP_LOGW(TAG, "Zigbee: parent-link probe has no cached sensor value");
        link_probe_handle_local_submit_failure(sequence, "no cached value for");
        return;
    }
    asdu[1] = sequence;

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

    err = esp_zb_aps_data_request(&req);
    if (err == ESP_OK) {
        s_link_probe_sent++;
        ESP_LOGW(TAG,
                 "Zigbee: active parent-link probe #%lu sent seq=%u cluster=0x%04x",
                 (unsigned long)s_link_probe_sent,
                 (unsigned)sequence, (unsigned)cluster_id);
        return;
    }

    ESP_LOGW(TAG, "Zigbee: active parent-link probe was rejected: %s",
             esp_err_to_name(err));
    link_probe_handle_local_submit_failure(sequence, "stack rejected");
}

static void link_probe_configure_verification(uint32_t timeout_ms,
                                               uint32_t retry_ms,
                                               uint8_t max_attempts)
{
    s_link_probe_timeout_ms = timeout_ms;
    s_link_probe_retry_ms = retry_ms;
    s_link_probe_max_attempts = max_attempts;
}

static void schedule_verification_timeout(void)
{
    esp_zb_scheduler_alarm_cancel(device_unavailable_verify_timer_cb, 0);
    esp_zb_scheduler_alarm(device_unavailable_verify_timer_cb, 0,
                           s_link_probe_timeout_ms);
}

static void schedule_device_unavailable_verify(void)
{
    schedule_verification_timeout();
    // The zero-delay scheduler callback performs the actual transmit in stack
    // context. Sensor sampling does not need to be involved.
    esp_zb_scheduler_alarm_cancel(link_probe_send_timer_cb, 0);
    esp_zb_scheduler_alarm(link_probe_send_timer_cb, 0, 0);
}

static void link_probe_start_verification(void)
{
    link_probe_configure_verification(ZB_UNAVAILABLE_VERIFY_DELAY_MS,
                                      LINK_PROBE_RETRY_MS,
                                      LINK_PROBE_MAX_ATTEMPTS);
    taskENTER_CRITICAL(&s_link_probe_lock);
    link_probe_reset_locked();
    s_link_probe_state = LINK_PROBE_ACTIVE;
    taskEXIT_CRITICAL(&s_link_probe_lock);
    schedule_device_unavailable_verify();
}

static void link_probe_start_candidate_verification(void)
{
    // A rejoin can be visible at NWK/BDB level before APS routing/security is
    // ready. Give the parent relationship a short settle period, then verify it
    // with sparse single-attribute probes instead of an immediate three-frame
    // sensor batch and aggressive APS repairs.
    link_probe_configure_verification(ZB_REJOIN_CANDIDATE_VERIFY_DELAY_MS,
                                      ZB_REJOIN_CANDIDATE_PROBE_RETRY_MS,
                                      ZB_REJOIN_CANDIDATE_PROBE_MAX_ATTEMPTS);
    taskENTER_CRITICAL(&s_link_probe_lock);
    link_probe_reset_locked();
    s_link_probe_state = LINK_PROBE_ACTIVE;
    s_link_probe_candidate = true;
    s_candidate_rejoin_total++;
    taskEXIT_CRITICAL(&s_link_probe_lock);
    esp_zb_scheduler_alarm_cancel(link_probe_send_timer_cb, 0);
    schedule_verification_timeout();
    esp_zb_scheduler_alarm(link_probe_send_timer_cb, 0,
                           ZB_REJOIN_CANDIDATE_FIRST_PROBE_MS);
}

static bool node_short_address_is_usable(void)
{
    uint16_t short_addr = esp_zb_get_short_address();

    // 0x0000 is the coordinator; 0xFFFE/0xFFFF mean "not assigned".
    return short_addr != 0x0000 && short_addr != 0xFFFE && short_addr != 0xFFFF;
}

static uint32_t zigbee_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool commissioning_event_is_recent(uint32_t timestamp_ms,
                                          uint32_t window_ms)
{
    return timestamp_ms != 0 &&
           (uint32_t)(zigbee_uptime_ms() - timestamp_ms) <= window_ms;
}

static esp_err_t zigbee_start_commissioning(uint8_t mode_mask)
{
    if (mode_mask == ESP_ZB_BDB_MODE_NETWORK_STEERING) {
        s_last_steering_attempt_ms = zigbee_uptime_ms();
    }
    return esp_zb_bdb_start_top_level_commissioning(mode_mask);
}

static bool commissioning_status_has_usable_network(
    esp_zb_bdb_commissioning_status_t bdb_status)
{
    // The stack retains the old short address after link loss, so a valid
    // address alone is not proof while a forced recovery is in progress.
    if (!node_short_address_is_usable()) {
        return false;
    }

    if (!s_link_resteer_forced) {
        // ON_A_NETWORK means BDB refused a redundant steering call.
        // DEV_ANNCE_SEND_FAILURE commonly follows a successful NWK rejoin whose
        // best-effort device announce was not acknowledged.
        return bdb_status == ESP_ZB_BDB_STATUS_ON_A_NETWORK ||
               bdb_status == ESP_ZB_BDB_STATUS_DEV_ANNCE_SEND_FAILURE;
    }

    // During forced recovery accept status 14 only as a candidate, and only if
    // it follows an application-issued steering attempt. It is still verified
    // by normal APS traffic / the explicit signal-60 probe before being trusted
    // for long.
    return bdb_status == ESP_ZB_BDB_STATUS_DEV_ANNCE_SEND_FAILURE &&
           commissioning_event_is_recent(s_last_steering_attempt_ms,
                                         ZIGBEE_RECENT_STEERING_ATTEMPT_MS);
}

static bool commissioning_status_requires_candidate_verification(
    esp_zb_bdb_commissioning_status_t bdb_status)
{
    // Both a fresh silent rejoin and a forced recovery can report NWK success
    // followed by DEV_ANNCE_SEND_FAILURE. Validate that new APS path with the
    // longer candidate window; forced recovery is allowed here only after an
    // application-issued steering attempt.
    if (bdb_status != ESP_ZB_BDB_STATUS_DEV_ANNCE_SEND_FAILURE ||
        !node_short_address_is_usable()) {
        return false;
    }

    return !s_link_resteer_forced ||
           commissioning_event_is_recent(s_last_steering_attempt_ms,
                                         ZIGBEE_RECENT_STEERING_ATTEMPT_MS);
}

static void zigbee_accept_commissioning_network(
    esp_zb_bdb_commissioning_status_t bdb_status,
    const char *reason)
{
    bool verify_candidate =
        commissioning_status_requires_candidate_verification(bdb_status);

    zigbee_mark_connected(reason);
    if (verify_candidate) {
        ESP_LOGI(TAG,
                 "Zigbee: provisional rejoin accepted; verify with sparse APS probe");
        link_probe_start_candidate_verification();
    }
}

static bool late_commissioning_failure_while_connected(
    esp_zb_bdb_commissioning_status_t bdb_status)
{
    // A failed scan started before the successful rejoin can deliver its
    // NO_NETWORK/IN_PROGRESS stop after the new link is already active. Ignore
    // those stale stops only in the short window immediately following a fresh
    // join candidate. Real parent outages still arrive through the explicit
    // device-unavailable path and are verified by active APS probes.
    return s_zigbee_connected &&
           commissioning_event_is_recent(s_rejoin_candidate_ms,
                                         ZIGBEE_STALE_COMMISSIONING_WINDOW_MS) &&
           (bdb_status == ESP_ZB_BDB_STATUS_IN_PROGRESS ||
            bdb_status == ESP_ZB_BDB_STATUS_NO_NETWORK ||
            bdb_status == ESP_ZB_BDB_STATUS_CANCELLED);
}

static void zigbee_mark_connected(const char *reason)
{
    s_link_resteer_forced = false;
    s_network_steering_pending = false;
    link_probe_cancel_active_preserve_retired();
    esp_zb_scheduler_alarm_cancel(device_unavailable_verify_timer_cb, 0);
    esp_zb_scheduler_alarm_cancel(link_probe_send_timer_cb, 0);
    s_zigbee_connected = true;
    s_bdb_busy_since_ms = 0;
    s_rejoin_candidate_ms = zigbee_uptime_ms();
    s_short_addr = esp_zb_get_short_address();
    esp_zb_scheduler_alarm_cancel(network_steering_timer_cb,
                                  ESP_ZB_BDB_MODE_NETWORK_STEERING);
    ESP_LOGI(TAG, "Zigbee: connected (%s); short addr=0x%04x", reason, s_short_addr);
}

static void network_steering_timer_cb(uint8_t mode_mask)
{
    esp_zb_bdb_commissioning_status_t bdb_status;
    esp_err_t err;

    s_network_steering_pending = false;
    bdb_status = esp_zb_get_bdb_commissioning_status();
    ESP_LOGI(TAG, "Zigbee: steering timer, bdb status=%d forced=%d",
             (int)bdb_status, (int)s_link_resteer_forced);

    if (bdb_status == ESP_ZB_BDB_STATUS_IN_PROGRESS) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (s_bdb_busy_since_ms == 0) {
            s_bdb_busy_since_ms = now_ms;
        }

        if ((uint32_t)(now_ms - s_bdb_busy_since_ms) < ZIGBEE_BDB_BUSY_RETRY_MS) {
            ESP_LOGI(TAG,
                     "Zigbee: commissioning still in progress; check again in %d ms",
                     ZIGBEE_STEERING_FAIL_RETRY_MS);
            schedule_network_steering(ZIGBEE_STEERING_FAIL_RETRY_MS);
            return;
        }

        ESP_LOGW(TAG,
                 "Zigbee: commissioning stuck in progress for %d ms; make one controlled retry",
                 ZIGBEE_BDB_BUSY_RETRY_MS);
        s_bdb_busy_since_ms = 0;
        err = zigbee_start_commissioning(mode_mask);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee: controlled commissioning retry accepted");
        } else {
            ESP_LOGW(TAG, "Zigbee: controlled commissioning retry rejected: %s",
                     esp_err_to_name(err));
        }
        schedule_network_steering(ZIGBEE_STEERING_FAIL_RETRY_MS);
        return;
    }

    s_bdb_busy_since_ms = 0;
    if (commissioning_status_has_usable_network(bdb_status)) {
        zigbee_accept_commissioning_network(bdb_status, "already on network");
        return;
    }

    err = zigbee_start_commissioning(mode_mask);
    if (err != ESP_OK) {
        bdb_status = esp_zb_get_bdb_commissioning_status();
        ESP_LOGW(TAG, "Zigbee: steering start failed: %s; bdb status=%d",
                 esp_err_to_name(err), (int)bdb_status);
        if (commissioning_status_has_usable_network(bdb_status)) {
            zigbee_accept_commissioning_network(bdb_status, "steering rejected");
        } else if (bdb_status == ESP_ZB_BDB_STATUS_IN_PROGRESS) {
            ESP_LOGI(TAG,
                     "Zigbee: commissioning remained in progress after steering request; check again in %d ms",
                     ZIGBEE_STEERING_FAIL_RETRY_MS);
            schedule_network_steering(ZIGBEE_STEERING_FAIL_RETRY_MS);
        } else {
            schedule_network_steering(ZIGBEE_STEERING_FAIL_RETRY_MS);
        }
    }
}

static void schedule_network_steering(uint32_t delay_ms)
{
    if (s_network_steering_pending) {
        ESP_LOGI(TAG, "Zigbee: network steering already pending");
        return;
    }

    s_network_steering_pending = true;
    esp_zb_scheduler_alarm_cancel(network_steering_timer_cb,
                                  ESP_ZB_BDB_MODE_NETWORK_STEERING);
    esp_zb_scheduler_alarm(network_steering_timer_cb,
                           ESP_ZB_BDB_MODE_NETWORK_STEERING,
                           delay_ms);
}

static void zigbee_link_lost(esp_zb_app_signal_type_t signal, esp_err_t status)
{
    ESP_LOGW(TAG,
             "Zigbee: link lost signal=%d status=0x%x; mark offline and steer again",
             (int)signal, (unsigned)status);
    link_probe_cancel_active_preserve_retired();
    esp_zb_scheduler_alarm_cancel(device_unavailable_verify_timer_cb, 0);
    esp_zb_scheduler_alarm_cancel(link_probe_send_timer_cb, 0);
    s_link_resteer_forced = true;
    s_zigbee_connected = false;
    s_bdb_busy_since_ms = 0;
    s_last_steering_attempt_ms = 0;
    s_rejoin_candidate_ms = 0;
    s_short_addr = 0;
    report_window_close();
    schedule_network_steering(ZIGBEE_LINK_RESTEER_DELAY_MS);
}

static void zigbee_handle_device_unavailable(uint32_t *signal_p,
                                             esp_err_t status)
{
    const node_zb_device_unavailable_params_t *params =
        (const node_zb_device_unavailable_params_t *)
            esp_zb_app_signal_get_params(signal_p);

    if (params != NULL && params->short_addr != COORDINATOR_SHORT_ADDR) {
        ESP_LOGI(TAG,
                 "Zigbee: ignore device-unavailable for non-parent 0x%04x",
                 params->short_addr);
        return;
    }

    uint32_t event_count = ++s_device_unavailable_total;
    if (!s_zigbee_connected) {
        // A steering alarm may already be pending; make sure it is not lost.
        schedule_network_steering(ZIGBEE_LINK_RESTEER_DELAY_MS);
        return;
    }

    taskENTER_CRITICAL(&s_link_probe_lock);
    if (s_link_probe_state == LINK_PROBE_ACTIVE) {
        taskEXIT_CRITICAL(&s_link_probe_lock);
        ESP_LOGW(TAG,
                 "Zigbee: parent unavailable event=%lu while probe verification is pending",
                 (unsigned long)event_count);
        return;
    }
    taskEXIT_CRITICAL(&s_link_probe_lock);

    s_link_probe_requests++;
    // Detach any ordinary reporting batch. Its late confirmations must not
    // affect probe verdict or trigger full-cache repairs during verification.
    report_window_close();
    if (s_sensor_task_handle != NULL) {
        xTaskNotify(s_sensor_task_handle,
                    SENSOR_NOTIFY_LINK_PROBE_BIT, eSetBits);
    }
    link_probe_start_verification();
    ESP_LOGW(TAG,
             "Zigbee: parent unavailable event=%lu; send active probe, verify in %lu ms",
             (unsigned long)event_count,
             (unsigned long)s_link_probe_timeout_ms);
}

static void device_unavailable_verify_timer_cb(uint8_t unused)
{
    bool expired = false;
    TickType_t now = xTaskGetTickCount();

    (void)unused;

    taskENTER_CRITICAL(&s_link_probe_lock);
    if (s_link_probe_state == LINK_PROBE_ACTIVE) {
        expired = true;
        if (s_link_probe_in_flight) {
            link_probe_retire_locked(s_link_probe_expected_sequence, now);
        }
        link_probe_reset_locked();
    }
    taskEXIT_CRITICAL(&s_link_probe_lock);

    if (!expired) {
        return;
    }
    if (!s_zigbee_connected) {
        return;
    }

    s_device_unavailable_verify_resteers++;
    ESP_LOGW(TAG,
             "Zigbee: parent unavailable probe had no APS success; resteer now (verify_resteers=%lu)",
             (unsigned long)s_device_unavailable_verify_resteers);
    zigbee_link_lost(ESP_ZB_ZDO_DEVICE_UNAVAILABLE, ESP_OK);
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
                zigbee_start_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                zigbee_mark_connected("device reboot");
            }
        } else {
            esp_zb_bdb_commissioning_status_t bdb_status =
                esp_zb_get_bdb_commissioning_status();
            ESP_LOGW(TAG,
                     "Zigbee: Startup/rejoin failed (0x%x), bdb status=%d; retry steering",
                     (unsigned)err_status, (int)bdb_status);
            if (commissioning_status_has_usable_network(bdb_status)) {
                zigbee_accept_commissioning_network(bdb_status,
                                                    "rejoin APS verification");
            } else if (late_commissioning_failure_while_connected(bdb_status)) {
                ESP_LOGI(TAG,
                         "Zigbee: ignore late reboot commissioning status=%d while connected",
                         (int)bdb_status);
            } else if (sig_type == ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT) {
                // A previously commissioned ZED first attempts a silent rejoin.
                // If that parent/rejoin attempt fails without retaining a usable
                // network address, explicitly run network steering instead of
                // waiting forever for another signal.
                schedule_network_steering(ZIGBEE_LINK_RESTEER_DELAY_MS);
            }
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee: Connected to network!");
            zigbee_mark_connected("network steering");
        } else {
            esp_zb_bdb_commissioning_status_t bdb_status =
                esp_zb_get_bdb_commissioning_status();
            ESP_LOGW(TAG,
                     "Zigbee: Steering failed (0x%x), bdb status=%d; retry in 5s",
                     (unsigned)err_status, (int)bdb_status);
            if (commissioning_status_has_usable_network(bdb_status)) {
                zigbee_accept_commissioning_network(bdb_status,
                                                    "steering bdb status");
            } else if (late_commissioning_failure_while_connected(bdb_status)) {
                ESP_LOGI(TAG,
                         "Zigbee: ignore late steering commissioning status=%d while connected",
                         (int)bdb_status);
            } else {
                schedule_network_steering(ZIGBEE_STEERING_FAIL_RETRY_MS);
            }
        }
        break;
    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        zigbee_link_lost(sig_type, err_status);
        break;
    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        zigbee_handle_device_unavailable(p_sg_p, err_status);
        break;
    case ESP_ZB_NLME_STATUS_INDICATION:
        // A healthy centralized network can emit periodic NWK status/address
        // verification indications. Link loss is handled by the explicit
        // signals above, so do not spam the log every few seconds here.
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

static bool aps_confirm_is_link_probe_frame(
    const esp_zb_apsde_data_confirm_t *confirm)
{
    return confirm->asdu != NULL
        && confirm->asdu_length >= (3 + 2 + 1 + 2)
        && confirm->asdu[0] == ZCL_FRAME_REPORT_SERVER_TO_CLIENT
        && confirm->asdu[2] == ZCL_CMD_REPORT_ATTRIBUTES
        && aps_confirm_is_sensor_report(confirm);
}

static bool aps_confirm_consume_link_probe(
    const esp_zb_apsde_data_confirm_t *confirm)
{
    bool current_match = false;
    bool retired_match = false;
    bool current_success = false;
    bool was_candidate = false;
    bool retry = false;
    uint8_t sequence = 0;
    TickType_t now = xTaskGetTickCount();

    if (!aps_confirm_is_link_probe_frame(confirm)) {
        return false;
    }

    sequence = confirm->asdu[1];
    taskENTER_CRITICAL(&s_link_probe_lock);
    current_match = s_link_probe_state == LINK_PROBE_ACTIVE
        && s_link_probe_in_flight
        && s_link_probe_expected_sequence == sequence;
    if (current_match) {
        current_success = confirm->status == 0;
        was_candidate = s_link_probe_candidate;
        if (current_success) {
            link_probe_reset_locked();
        } else {
            bool can_retry = s_link_probe_attempts < s_link_probe_max_attempts;
            s_link_probe_in_flight = false;
            if (can_retry) {
                retry = true;
            }
        }
    } else {
        retired_match = link_probe_take_retired_locked(sequence, now);
    }
    taskEXIT_CRITICAL(&s_link_probe_lock);

    if (current_match && current_success) {
        esp_zb_scheduler_alarm_cancel(device_unavailable_verify_timer_cb, 0);
        if (was_candidate) {
            s_candidate_rejoin_verified++;
            ESP_LOGI(TAG,
                     "Zigbee: provisional rejoin verified by active probe seq=%u (%lu/%lu)",
                     (unsigned)sequence,
                     (unsigned long)s_candidate_rejoin_verified,
                     (unsigned long)s_candidate_rejoin_total);
        } else {
            s_device_unavailable_recovered++;
            ESP_LOGI(TAG,
                     "Zigbee: parent unavailable resolved by active probe seq=%u (total=%lu recovered=%lu)",
                     (unsigned)sequence,
                     (unsigned long)s_device_unavailable_total,
                     (unsigned long)s_device_unavailable_recovered);
        }
        return true;
    }

    if (current_match) {
        s_link_probe_aps_failures++;
        if (retry) {
            ESP_LOGW(TAG,
                     "Zigbee: %s probe seq=%u failed APS; retry %u/%u in %lu ms",
                     was_candidate ? "provisional rejoin" : "active parent-link",
                     (unsigned)sequence,
                     (unsigned)(s_link_probe_attempts + 1),
                     (unsigned)s_link_probe_max_attempts,
                     (unsigned long)s_link_probe_retry_ms);
            esp_zb_scheduler_alarm_cancel(link_probe_send_timer_cb, 0);
            esp_zb_scheduler_alarm(link_probe_send_timer_cb, 0,
                                   s_link_probe_retry_ms);
        } else {
            ESP_LOGW(TAG,
                     "Zigbee: active parent-link probe seq=%u failed APS after %u attempts; wait for %lu ms verification timeout",
                     (unsigned)sequence,
                     (unsigned)s_link_probe_max_attempts,
                     (unsigned long)s_link_probe_timeout_ms);
        }
        return true;
    }

    if (retired_match) {
        ESP_LOGI(TAG,
                 "Zigbee: ignore late retired parent-link probe confirm seq=%u status=%u",
                 (unsigned)sequence, (unsigned)confirm->status);
        return true;
    }

    return false;
}

// Runs in Zigbee stack context. Keep it short: only record the APS result and
// wake the sensor task. Never send another Zigbee frame directly from here.
static void aps_data_confirm_cb(esp_zb_apsde_data_confirm_t confirm)
{
    bool app_report = false;

    // Current probe results drive verification. Retired probe results are
    // swallowed after timeout so they can never trigger ordinary full-cache
    // repairs or consume a future reporting batch's in-flight slot.
    if (aps_confirm_consume_link_probe(&confirm)) {
        return;
    }

    if (aps_confirm_is_sensor_report(&confirm)) {
        taskENTER_CRITICAL(&s_report_state_lock);
        if (s_report_window_open && s_reports_in_flight > 0) {
            app_report = true;
            s_reports_in_flight--;
        }
        taskEXIT_CRITICAL(&s_report_state_lock);
    }

    if (app_report && confirm.status == 0) {
        s_aps_confirm_success++;
    } else if (app_report) {
        s_aps_confirm_failure++;
        if (s_sensor_task_handle != NULL) {
            xTaskNotify(s_sensor_task_handle,
                        SENSOR_NOTIFY_APS_FAIL_BIT, eSetBits);
        }
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
    uint8_t report_sequence;
    if (!zcl_report_sequence_allocate(&report_sequence)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t asdu[3 + 2 + 1 + 2] = {
        ZCL_FRAME_REPORT_SERVER_TO_CLIENT,
        report_sequence,
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

    if (!s_zigbee_connected || link_probe_blocks_reports()) {
        ESP_LOGI(TAG, "REPORT_SEND: skipped reason=%s while Zigbee link is changing", reason);
        return false;
    }

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
             "i2c_resets=%lu i2c_reinits=%lu "
             "attempts=%lu queued=%lu queue_fail=%lu items_failed=%lu aps_ok=%lu aps_fail=%lu "
             "unavail=%lu recovered=%lu candidate=%lu/%lu probe_resteer=%lu probes=%lu/%lu probe_aps_fail=%lu "
             "th_invalid=%lu lux_invalid=%lu th_streak=%lu lux_streak=%lu",
             (unsigned long)s_report_rounds,
             (unsigned long)s_report_repairs,
             (unsigned long)s_i2c_th_failures,
             (unsigned long)s_i2c_lux_failures,
             (unsigned long)s_i2c_bus_resets,
             (unsigned long)s_i2c_bus_reinits,
             (unsigned long)s_report_attempts,
             (unsigned long)s_report_enqueued,
             (unsigned long)s_report_enqueue_failures,
             (unsigned long)s_report_items_failed,
             (unsigned long)s_aps_confirm_success,
             (unsigned long)s_aps_confirm_failure,
             (unsigned long)s_device_unavailable_total,
             (unsigned long)s_device_unavailable_recovered,
             (unsigned long)s_candidate_rejoin_verified,
             (unsigned long)s_candidate_rejoin_total,
             (unsigned long)s_device_unavailable_verify_resteers,
             (unsigned long)s_link_probe_sent,
             (unsigned long)s_link_probe_requests,
             (unsigned long)s_link_probe_aps_failures,
             (unsigned long)s_sensor_th_invalid,
             (unsigned long)s_sensor_lux_invalid,
             (unsigned long)s_th_fail_streak,
             (unsigned long)s_lux_fail_streak);
}

static void sensor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));

    bool aht20_ready = false;
    if (i2c_sensor_bus_ready()) {
        aht20_ready = aht20_init();
        ESP_LOGI(TAG, "AHT20 initial setup: %s", aht20_ready ? "OK" : "failed; will retry");
    } else {
        ESP_LOGE(TAG, "I2C: bus is not ready at sensor task start; will retry");
    }

    while (1) {
        if (!i2c_sensor_bus_ready()) {
            esp_err_t bus_err = i2c_sensor_bus_rebuild("bus was unavailable");
            if (bus_err != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            aht20_ready = aht20_init();
            ESP_LOGI(TAG, "I2C: bus recovered; AHT20 setup=%s",
                     aht20_ready ? "OK" : "failed; will retry");
        }

        if (!s_zigbee_connected) {
            ESP_LOGI(TAG, "Waiting for Zigbee network...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        xTaskNotifyWait(0, UINT32_MAX, NULL, 0);
        report_window_close();

        // Normal running-link verification owns the radio window. A candidate
        // rejoin deliberately allows real samples so APS can verify the path.
        if (link_probe_blocks_reports()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        float temp = 0, hum = 0, lux = 0;
        sensor_read_status_t th_status = SENSOR_READ_BUS_ERROR;
        if (aht20_ready) {
            th_status = aht20_read(&temp, &hum);
            if (th_status == SENSOR_READ_BUS_ERROR) {
                // Reinitialize on the next round after NACK/timeout. A timeout
                // has already attempted an I2C bus reset inside read.
                aht20_ready = false;
            }
        } else {
            aht20_ready = aht20_init();
            if (aht20_ready) {
                ESP_LOGI(TAG, "AHT20 setup recovered");
                // Do not charge a healthy setup round to the failure streak.
                th_status = aht20_read(&temp, &hum);
                if (th_status == SENSOR_READ_BUS_ERROR) {
                    aht20_ready = false;
                }
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
            uint32_t notify_value = 0;
            if ((int32_t)remaining <= 0) {
                break;
            }

            if (xTaskNotifyWait(0, UINT32_MAX, &notify_value, remaining) != pdTRUE) {
                break;  // Normal 10-second reporting period elapsed.
            }

            if (!s_zigbee_connected || link_probe_blocks_reports()) {
                ESP_LOGI(TAG, "REPORT_APS: stop repair while Zigbee link is changing");
                report_window_close();
                break;
            }

            if ((notify_value & SENSOR_NOTIFY_APS_FAIL_BIT) == 0) {
                continue;
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

        bool th_bus_error = th_status == SENSOR_READ_BUS_ERROR;
        bool lux_bus_error = lux_status == SENSOR_READ_BUS_ERROR;
        bool bus_reinit_due =
            (th_bus_error && s_th_fail_streak > 0 &&
             (s_th_fail_streak % I2C_BUS_REINIT_FAILURES) == 0) ||
            (lux_bus_error && s_lux_fail_streak > 0 &&
             (s_lux_fail_streak % I2C_BUS_REINIT_FAILURES) == 0);

        if (bus_reinit_due) {
            esp_err_t bus_err = i2c_sensor_bus_rebuild("consecutive sensor bus failures");
            aht20_ready = false;
            if (bus_err == ESP_OK) {
                ESP_LOGI(TAG, "I2C: scheduled bus rebuild succeeded; AHT20 will initialize next round");
            }
        }
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

    esp_err_t i2c_err = i2c_sensor_bus_init();
    if (i2c_err != ESP_OK) {
        // A damaged or temporarily unresponsive sensor bus must not prevent
        // Zigbee from starting; the sensor task keeps rebuilding the bus.
        ESP_LOGE(TAG, "I2C init unavailable at startup: %s; Zigbee will continue",
                 esp_err_to_name(i2c_err));
    } else {
        ESP_LOGI(TAG, "I2C init OK (SDA=%d SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);

        // Temporary wiring diagnostic: scan bus for AHT20 (0x38) / BH1750 (0x23)
        for (uint8_t a = 1; a < 127; a++) {
            if (i2c_master_probe(s_i2c_bus, a, I2C_SCAN_TIMEOUT_MS) == ESP_OK) {
                ESP_LOGW(TAG, "I2C SCAN: found 0x%02x", a);
            }
        }
        ESP_LOGW(TAG, "I2C SCAN: expect AHT20=0x38 BH1750=0x23");
    }

    // Create the sensor task first so its notification handle exists before
    // the Zigbee task registers the APS confirm callback.
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, &s_sensor_task_handle);
    xTaskCreate(zigbee_task, "zigbee", 16384, NULL, 5, NULL);

    ESP_LOGI(TAG, "Zigbee sensor node ready!");
}
