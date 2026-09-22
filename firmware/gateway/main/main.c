/**
 * IoT-Home Gateway Firmware
 * 
 * Features:
 * - Wi-Fi STA connection with auto-reconnect
 * - MQTT client with auto-reconnect  
 * - OLED status display (SSD1306 128x64 I2C)
 * - Telemetry data upload
 * 
 * Hardware:
 * - ESP32-C6 SuperMini
 * - SSD1306 OLED 0.96" (I2C: SDA=GPIO2, SCL=GPIO3, addr=0x3C)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_attribute.h"
#include "nwk/esp_zigbee_nwk.h"
#include "aps/esp_zigbee_aps.h"
#include "esp_coexist.h"
#include "esp_ieee802154.h"
#include "wifi_secrets.h"
#include "mqtt_secrets.h"
#include "cJSON.h"
#include "zcl/esp_zigbee_zcl_on_off.h"

// ==================== Configuration ====================

#define WIFI_RETRY_MAX          1000    // ~8 hours of retries
#define WIFI_RETRY_BASE_MS      1000
#define WIFI_RETRY_MAX_MS       60000
#define WIFI_BOOT_GRACE_MS      25000   // if not associated this long after boot,
#define WIFI_BOOT_MAX_RESTARTS  2       // warm-reboot (caps cold-boot radio glitch)
#define WIFI_RF_EMPTY_LIMIT     3       // zero-AP scans before deep-sleep RF reset
#define WIFI_RF_SLEEP_US        (2000000ULL)

#define MQTT_BROKER_URI         "mqtt://8.163.110.27:1883"
#define MQTT_TOPIC_PREFIX       "iot-home/gw-001"
#define MQTT_GW_STATUS_SUFFIX   "nodes/gw-001/status"

#define I2C_SDA_PIN             2
#define I2C_SCL_PIN             3
#define OLED_ADDR               0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64

#define STATUS_INTERVAL_MS      30000
#define OLED_UPDATE_MS          1000

#define RF_POWER_CLUSTER_ID     0xFC10
#define RF_PACKET_MAGIC0        0x52
#define RF_PACKET_MAGIC1        0x46
#define RF_PACKET_VERSION       2
#define RF_PACKET_V1_LEN        6
#define RF_PACKET_LEN           8
#define RF_CMD_QUERY            1
#define RF_CMD_SET              2
#define RF_CMD_REPORT           3
#define RF_MODE_AUTO            0
#define RF_MODE_MANUAL          1
#define RF_MODE_UNKNOWN         0xFF
#define RF_POWER_UNKNOWN_DBM       (-128)
// One-time installation bounds. Values must exist in s_rf_power_table below.
// The controller still starts/reconciles from the node and does not use
// distance as a direct control input.
#define RF_POWER_AUTO_MIN_DBM     (-10)
#define RF_POWER_AUTO_MAX_DBM     (20)
#define RF_POWER_LEVEL_IS_SUPPORTED(p) \
    (((p) == -10) || ((p) == 0) || ((p) == 8) || ((p) == 14) || \
     ((p) == 18) || ((p) == 20))
_Static_assert(RF_POWER_LEVEL_IS_SUPPORTED(RF_POWER_AUTO_MIN_DBM),
               "RF_POWER_AUTO_MIN_DBM must exactly match a power-table level");
_Static_assert(RF_POWER_LEVEL_IS_SUPPORTED(RF_POWER_AUTO_MAX_DBM),
               "RF_POWER_AUTO_MAX_DBM must exactly match a power-table level");
#define RF_POWER_EVAL_MS          60000
#define RF_POWER_FRESH_MS         120000
#define RF_POWER_SAMPLES_NEEDED   6
// Use LQI only as a diagnostic/down-control signal. On this ESP32-C6 setup,
// successful close-range APS traffic can report LQI around 20 while stale RSSI
// can read near -94 dBm, so weak-looking LQI alone must not raise power. Real
// commissioning/APS failures drive power-up; persistent very-high LQI can lower
// an unnecessarily high learned level.
#define RF_LQI_LOWER              240
#define RF_LQI_LOWER_WINDOWS      3
#define RF_POWER_SETTLE_MS        90000
#define RF_POWER_RECONCILE_MS     300000
#define RF_ZB_ENDPOINT            10

// Forward declarations
void mqtt_pub(const char *topic_suffix, const char *data);
void mqtt_pub_retained(const char *topic_suffix, const char *data, bool retain);

// ==================== Globals ====================

static const char *TAG = "iot-gw";

#define WIFI_CONNECTED_BIT  BIT0
#define MQTT_CONNECTED_BIT  BIT2

static EventGroupHandle_t s_event_group;
static int s_wifi_retry = 0;
static TaskHandle_t s_wifi_reconn_task = NULL;
static volatile bool s_wifi_ever_connected = false;
static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool s_wifi_ok = false;
static bool s_mqtt_ok = false;

// Outage buffer for non-retained telemetry (definition of storage and
// helpers follows below, after mqtt_start()).
#define MQTT_OUTBOX_LEN 30
typedef struct {
    char topic_suffix[64];
    char payload[192];
} mqtt_outbox_msg_t;
static QueueHandle_t s_mqtt_outbox;
static StaticQueue_t s_mqtt_outbox_struct;
static mqtt_outbox_msg_t s_mqtt_outbox_storage[MQTT_OUTBOX_LEN];
static void mqtt_outbox_store(const char *topic_suffix, const char *data);

static char s_ip[16] = "0.0.0.0";
static bool s_network_formed = false;
static bool s_child_seen = false;
static bool s_zigbee_ok = false;
static volatile int s_zigbee_devices = 0;
static volatile bool s_zb_status_resync = false;
static bool s_oled_available = false;

// ZBOSS may deliver some signals with task preemption disabled.  Keep those
// callbacks free of ESP_LOG/newlib/MQTT and hand a small copied event to a
// normal task.  Recursive VFS locks (USB serial logging) in that context call
// abort() with "recursive mutexes make no sense in ISR context".
typedef enum {
    ZB_APP_EVENT_JOIN = 1,
    ZB_APP_EVENT_REPORT,
    ZB_APP_EVENT_LEAVE,
    ZB_APP_EVENT_SIGNAL,
} zb_app_event_type_t;

typedef struct {
    zb_app_event_type_t type;
    uint16_t addr;
    uint16_t cluster;
    uint16_t raw;
    uint8_t ieee[8];
    uint8_t signal;
    uint8_t value;
    int32_t status;
} zb_app_event_t;

_Static_assert((int)ESP_ZB_SIGNAL_END <= 0xff,
               "Widen zb_app_event_t.signal for the installed Zigbee SDK");

#define ZB_APP_QUEUE_LEN            32
#define ZB_SIGNAL_QUEUE_LEN         16

static QueueHandle_t s_zb_app_q;
static QueueHandle_t s_zb_signal_q;
static QueueSetHandle_t s_zb_queue_set;
static uint32_t s_zb_report_events;
// Keep task- and ISR-like-context drops separate so the counters themselves do
// not need a lock or non-atomic read-modify-write.
static volatile uint32_t s_zb_app_drops_task;
static volatile uint32_t s_zb_app_drops_isr;
static volatile uint32_t s_zb_signal_drops_task;
static volatile uint32_t s_zb_signal_drops_isr;

// ZBOSS may invoke callbacks from task or restricted/ISR-like context.
// Non-blocking delivery must never stall the stack; count rare queue drops.
static bool zb_queue_try_send(QueueHandle_t queue,
                              const zb_app_event_t *event,
                              volatile uint32_t *task_drops,
                              volatile uint32_t *isr_drops)
{
    bool from_isr = !xPortCanYield();

    if (queue == NULL) {
        if (from_isr) {
            (*isr_drops)++;
        } else {
            (*task_drops)++;
        }
        return false;
    }

    bool queued;
    if (from_isr) {
        BaseType_t higher_woken = pdFALSE;
        queued = xQueueSendFromISR(queue, event, &higher_woken) == pdTRUE;
        portYIELD_FROM_ISR(higher_woken);
    } else {
        queued = xQueueSend(queue, event, 0) == pdTRUE;
    }

    if (!queued) {
        if (from_isr) {
            (*isr_drops)++;
        } else {
            (*task_drops)++;
        }
    }
    return queued;
}

static bool zb_app_event_send(const zb_app_event_t *event)
{
    return zb_queue_try_send(s_zb_app_q, event,
                             &s_zb_app_drops_task,
                             &s_zb_app_drops_isr);
}

// Diagnostic events use a separate queue. Even a pathological signal storm can
// only discard diagnostics; join/report/leave events keep their own capacity.
static bool zb_signal_event_send(const zb_app_event_t *event)
{
    return zb_queue_try_send(s_zb_signal_q, event,
                             &s_zb_signal_drops_task,
                             &s_zb_signal_drops_isr);
}

// Some ZBOSS callbacks run with scheduling/preemption restricted. Keep them
// free of logging/VFS and defer diagnostic output to zb_forward_task().
static void zb_app_signal_event_send(esp_zb_app_signal_type_t signal,
                                     esp_err_t status,
                                     uint8_t value)
{
    zb_app_event_t event = {
        .type = ZB_APP_EVENT_SIGNAL,
        .signal = (uint8_t)signal,
        .status = (int32_t)status,
        .value = value,
    };
    (void)zb_signal_event_send(&event);
}

// Minimal ABI-compatible view of the private ZBOSS NLME payload for the
// installed esp-zboss-lib version. Avoid including internal ZBOSS headers
// from application code. The short address is read byte-by-byte so the packed
// offset-one field never generates an unaligned scalar access.
typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t network_addr_le[2];
    uint8_t unknown_command_id;
} gw_zb_nlme_status_indication_t;

typedef struct {
    gw_zb_nlme_status_indication_t nlme_status;
} gw_zb_nlme_status_signal_params_t;

_Static_assert(sizeof(gw_zb_nlme_status_indication_t) == 4,
               "NLME status indication ABI size mismatch");
_Static_assert(offsetof(gw_zb_nlme_status_indication_t, status) == 0,
               "NLME status offset mismatch");
_Static_assert(offsetof(gw_zb_nlme_status_indication_t,
                        network_addr_le) == 1,
               "NLME network address offset mismatch");
_Static_assert(offsetof(gw_zb_nlme_status_indication_t,
                        unknown_command_id) == 3,
               "NLME command id offset mismatch");
_Static_assert(sizeof(gw_zb_nlme_status_signal_params_t) == 4,
               "NLME signal params wrapper size mismatch");

// NLME indications can repeat on a healthy small network. Keep at most one
// non-zero indication per minute even if distinct status codes alternate.
static bool zb_nlme_diagnostic_allowed(void)
{
    static TickType_t last_kept_tick = 0;
    TickType_t now = xPortCanYield() ? xTaskGetTickCount()
                                     : xTaskGetTickCountFromISR();

    if ((TickType_t)(now - last_kept_tick) >= pdMS_TO_TICKS(60000)) {
        last_kept_tick = now;
        return true;
    }
    return false;
}

// ==================== Gateway-side automatic TX power control ====================

static const int8_t s_rf_power_table[] = {-10, 0, 8, 14, 18, 20};
#define RF_POWER_LEVEL_COUNT (sizeof(s_rf_power_table) / sizeof(s_rf_power_table[0]))

static uint16_t s_rf_node_addr;
static int s_rf_lqi_ema = -1;
static uint8_t s_rf_lqi_samples;
static uint32_t s_rf_last_rx_ms;
static int8_t s_rf_node_power_dbm = RF_POWER_UNKNOWN_DBM;
static uint8_t s_rf_node_mode = RF_MODE_UNKNOWN;
static uint8_t s_rf_packet_sequence;
static uint32_t s_rf_last_eval_ms;
static uint8_t s_rf_lqi_low_windows;
static uint8_t s_rf_lqi_high_windows;
static bool s_rf_report_waiting;
static uint8_t s_rf_expected_report_seq;
static int8_t s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
static uint8_t s_rf_expected_report_mode = RF_MODE_UNKNOWN;
static bool s_rf_last_report_seq_valid;
static uint8_t s_rf_last_report_seq;
static uint32_t s_rf_wait_started_ms;
static uint32_t s_rf_power_confirmed_ms;
static uint8_t s_rf_mode;
static int8_t s_rf_manual_power_dbm;
static bool s_rf_policy_apply_pending;
static char s_rf_expected_request_id[17];
static bool s_rf_expected_user_transaction;
static bool s_rf_policy_user_transaction;
static char s_rf_policy_request_id[17];
static portMUX_TYPE s_rf_power_lock = portMUX_INITIALIZER_UNLOCKED;

static bool rf_power_known_active_child(uint16_t addr);
static bool rf_power_is_table_value(int8_t power_dbm);
static bool rf_power_request_id_valid(const char *request_id);

static bool rf_power_is_monitored_cluster(uint16_t cluster_id)
{
    return cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT ||
           cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT ||
           cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT ||
           cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF ||
           cluster_id == RF_POWER_CLUSTER_ID;
}

static bool rf_power_is_table_value(int8_t power_dbm)
{
    for (uint8_t i = 0; i < RF_POWER_LEVEL_COUNT; ++i) {
        if (s_rf_power_table[i] == power_dbm) {
            return true;
        }
    }
    return false;
}

static bool rf_power_is_allowed_value(int8_t power_dbm)
{
    return power_dbm >= RF_POWER_AUTO_MIN_DBM &&
           power_dbm <= RF_POWER_AUTO_MAX_DBM &&
           rf_power_is_table_value(power_dbm);
}

static uint8_t rf_power_nearest_index(int8_t power_dbm)
{
    uint8_t best = 0;
    uint16_t best_delta = 0xFFFF;

    for (uint8_t i = 0; i < RF_POWER_LEVEL_COUNT; ++i) {
        int delta = (int)power_dbm - (int)s_rf_power_table[i];
        uint16_t abs_delta = (uint16_t)(delta < 0 ? -delta : delta);
        if (abs_delta < best_delta) {
            best_delta = abs_delta;
            best = i;
        }
    }
    return best;
}

static uint8_t rf_power_min_index(void)
{
    return rf_power_nearest_index(RF_POWER_AUTO_MIN_DBM);
}

static uint8_t rf_power_max_index(void)
{
    return rf_power_nearest_index(RF_POWER_AUTO_MAX_DBM);
}

#define RF_POLICY_NVS_NAMESPACE     "rfpol"

static const char *rf_power_mode_name(uint8_t mode)
{
    if (mode == RF_MODE_MANUAL) {
        return "manual";
    }
    if (mode == RF_MODE_AUTO) {
        return "auto";
    }
    return "unknown";
}

static bool rf_power_policy_values_valid(uint8_t mode, int8_t manual_power_dbm)
{
    return (mode == RF_MODE_AUTO || mode == RF_MODE_MANUAL) &&
           rf_power_is_allowed_value(manual_power_dbm);
}

static void rf_power_policy_persist(uint8_t mode, int8_t manual_power_dbm)
{
    nvs_handle_t handle;

    if (nvs_open(RF_POLICY_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "RF power policy persistence namespace unavailable");
        return;
    }

    esp_err_t err = nvs_set_u8(handle, "mode", mode);
    if (err == ESP_OK) {
        err = nvs_set_i8(handle, "mpwr", manual_power_dbm);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RF power policy persistence failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static void rf_set_policy_request_id_locked(const char *request_id)
{
    s_rf_policy_request_id[0] = '\0';
    if (rf_power_request_id_valid(request_id)) {
        strlcpy(s_rf_policy_request_id, request_id,
                sizeof(s_rf_policy_request_id));
    }
}

static void rf_power_policy_init(void)
{
    uint8_t mode = RF_MODE_AUTO;
    int8_t manual_power_dbm = RF_POWER_AUTO_MIN_DBM;
    nvs_handle_t handle;

    if (nvs_open(RF_POLICY_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t stored_mode = RF_MODE_AUTO;
        int8_t stored_manual_power = RF_POWER_AUTO_MIN_DBM;
        esp_err_t mode_err = nvs_get_u8(handle, "mode", &stored_mode);
        esp_err_t power_err = nvs_get_i8(handle, "mpwr", &stored_manual_power);
        nvs_close(handle);

        if (mode_err == ESP_OK && power_err == ESP_OK &&
            rf_power_policy_values_valid(stored_mode, stored_manual_power)) {
            mode = stored_mode;
            manual_power_dbm = stored_manual_power;
        } else {
            ESP_LOGW(TAG, "RF power policy storage invalid; using defaults");
        }
    }

    s_rf_mode = mode;
    s_rf_manual_power_dbm = manual_power_dbm;
    s_rf_policy_apply_pending = false;
    s_rf_policy_user_transaction = false;
    s_rf_policy_request_id[0] = '\0';
    ESP_LOGI(TAG, "RF power policy: mode=%s manual=%d dBm",
             rf_power_mode_name(mode), (int)manual_power_dbm);
}

static void rf_power_forget_node(uint16_t addr)
{
    taskENTER_CRITICAL(&s_rf_power_lock);
    if (s_rf_node_addr == addr) {
        s_rf_node_addr = 0;
        s_rf_lqi_ema = -1;
        s_rf_lqi_samples = 0;
        s_rf_last_rx_ms = 0;
        s_rf_node_power_dbm = RF_POWER_UNKNOWN_DBM;
        s_rf_node_mode = RF_MODE_UNKNOWN;
        s_rf_lqi_low_windows = 0;
        s_rf_lqi_high_windows = 0;
        s_rf_report_waiting = false;
        s_rf_expected_report_seq = 0;
        s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
        s_rf_expected_report_mode = RF_MODE_UNKNOWN;
        s_rf_last_report_seq_valid = false;
        s_rf_last_report_seq = 0;
        s_rf_wait_started_ms = 0;
        s_rf_power_confirmed_ms = 0;
        s_rf_policy_apply_pending = false;
        s_rf_expected_request_id[0] = '\0';
        s_rf_expected_user_transaction = false;
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);
}

static void rf_power_reset_for_announce(uint16_t addr)
{
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    // A device announce can be a fresh node as well as a reset of the same short
    // address. Discard any power/LQI state from the previous process and force
    // the next periodic pass to QUERY. Otherwise the gateway could apply a
    // stale high-power level to a node that booted safely at -10 dBm.
    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_node_addr = addr;
    s_rf_lqi_ema = -1;
    s_rf_lqi_samples = 0;
    s_rf_last_rx_ms = now;
    s_rf_node_power_dbm = RF_POWER_UNKNOWN_DBM;
    s_rf_node_mode = RF_MODE_UNKNOWN;
    s_rf_lqi_low_windows = 0;
    s_rf_lqi_high_windows = 0;
    s_rf_report_waiting = false;
    s_rf_expected_report_seq = 0;
    s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
    s_rf_expected_report_mode = RF_MODE_UNKNOWN;
    s_rf_last_report_seq_valid = false;
    s_rf_last_report_seq = 0;
    s_rf_wait_started_ms = 0;
    s_rf_power_confirmed_ms = 0;
    s_rf_policy_apply_pending = s_rf_policy_user_transaction ||
                                 (s_rf_mode == RF_MODE_MANUAL);
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    if (!s_rf_policy_user_transaction) {
        s_rf_policy_request_id[0] = '\0';
    }
    s_rf_last_eval_ms = 0;
    taskEXIT_CRITICAL(&s_rf_power_lock);
}

static bool rf_power_report_is_new_unsolicited_locked(uint8_t seq)
{
    // Gateway-initiated replies echo a low sequence number and are accepted via
    // the exact expected-sequence path. Node-initiated reports use 128..255.
    if (seq < 0x80) {
        return false;
    }
    if (!s_rf_last_report_seq_valid) {
        return true;
    }
    if (s_rf_last_report_seq < 0x80) {
        // First node-initiated report after a gateway-driven exchange.
        return true;
    }

    uint8_t last_low = s_rf_last_report_seq & 0x7f;
    uint8_t current_low = seq & 0x7f;
    uint8_t forward_delta = (uint8_t)(current_low - last_low);
    return forward_delta >= 1 && forward_delta <= 127;
}

typedef enum {
    RF_REJECT_NONE = 0,
    RF_REJECT_UNSECURED,
    RF_REJECT_MALFORMED,
    RF_REJECT_UNEXPECTED_CMD,
    RF_REJECT_UNKNOWN_CHILD,
    RF_REJECT_INVALID_POWER,
    RF_REJECT_MISMATCH,
    RF_REJECT_OLD,
} rf_reject_reason_t;

static void rf_power_publish_state(uint16_t addr, int8_t power_dbm,
                                   uint8_t node_mode, const char *state,
                                   const char *request_id)
{
    char suffix[64];
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        return;
    }

    snprintf(suffix, sizeof(suffix), "nodes/zb-%04x/rf_policy",
             (unsigned)addr);
    cJSON_AddStringToObject(root, "mode",
                            rf_power_mode_name(node_mode));
    if (power_dbm == RF_POWER_UNKNOWN_DBM) {
        cJSON_AddNullToObject(root, "power_dbm");
    } else {
        cJSON_AddNumberToObject(root, "power_dbm", (int)power_dbm);
    }
    cJSON_AddStringToObject(root, "status", state);
    if (request_id == NULL || request_id[0] == '\0') {
        cJSON_AddNullToObject(root, "request_id");
    } else {
        cJSON_AddStringToObject(root, "request_id", request_id);
    }
    cJSON_AddStringToObject(root, "desired_mode",
                            rf_power_mode_name(s_rf_mode));
    cJSON_AddNumberToObject(root, "desired_manual_power_dbm",
                            (int)s_rf_manual_power_dbm);

    char *payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        mqtt_pub_retained(suffix, payload, true);
    }
    cJSON_free(payload);
    cJSON_Delete(root);
}

static void rf_power_publish_querying(uint16_t addr, const char *request_id)
{
    rf_power_publish_state(addr, RF_POWER_UNKNOWN_DBM,
                           RF_MODE_UNKNOWN, "querying", request_id);
}

static bool rf_power_request_id_valid(const char *request_id)
{
    size_t len = 0;

    if (request_id == NULL) {
        return false;
    }
    len = strlen(request_id);
    if (len == 0 || len >= sizeof(s_rf_expected_request_id)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char ch = request_id[i];
        bool ok = (ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void rf_power_set_request_id_locked(const char *request_id)
{
    s_rf_expected_request_id[0] = '\0';
    if (rf_power_request_id_valid(request_id)) {
        strlcpy(s_rf_expected_request_id, request_id,
                sizeof(s_rf_expected_request_id));
    }
}

static void rf_power_clear_waiting_locked(void)
{
    s_rf_report_waiting = false;
    s_rf_expected_report_seq = 0;
    s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
    s_rf_expected_report_mode = RF_MODE_UNKNOWN;
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    s_rf_wait_started_ms = 0;
}

// Called while s_rf_power_lock is held. Once the deadline passes, a late,
// otherwise exact REPORT must no longer confirm the original user transaction.
static bool rf_power_expire_waiting_if_due_locked(uint32_t now)
{
    if (!s_rf_report_waiting || s_rf_wait_started_ms == 0 ||
        now - s_rf_wait_started_ms < RF_POWER_SETTLE_MS) {
        return false;
    }

    rf_power_clear_waiting_locked();
    return true;
}

static bool rf_power_aps_indication_cb(esp_zb_apsde_data_ind_t ind)
{
    bool is_private = ind.cluster_id == RF_POWER_CLUSTER_ID;
    bool power_report = false;
    bool accept_report = false;
    bool packet_v1 = false;
    bool packet_v2 = false;
    int8_t reported_power = 0;
    uint8_t reported_mode = RF_MODE_UNKNOWN;
    uint8_t report_seq = 0;
    uint8_t expected_seq = 0;
    uint8_t expected_mode = RF_MODE_UNKNOWN;
    int8_t expected_power = RF_POWER_UNKNOWN_DBM;
    uint8_t last_seq = 0;
    bool expected_user = false;
    int8_t recent_rssi = -127;
    bool old_power_unknown = true;
    bool matched_transaction = false;
    char response_request_id[17] = {0};
    rf_reject_reason_t reject_reason = RF_REJECT_NONE;

    if (!rf_power_is_monitored_cluster(ind.cluster_id)) {
        return false;
    }

    if (ind.status != 0 || ind.profile_id != ESP_ZB_AF_HA_PROFILE_ID ||
        ind.src_endpoint != RF_ZB_ENDPOINT || ind.dst_endpoint != RF_ZB_ENDPOINT) {
        return is_private;
    }

    if (ind.security_status == 0) {
        reject_reason = RF_REJECT_UNSECURED;
        return is_private;
    }

    if (is_private) {
        if (ind.asdu == NULL) {
            reject_reason = RF_REJECT_MALFORMED;
            return true;
        }

        packet_v1 = ind.asdu_length == RF_PACKET_V1_LEN &&
                    ind.asdu[2] == 1;
        packet_v2 = ind.asdu_length == RF_PACKET_LEN &&
                    ind.asdu[2] == RF_PACKET_VERSION;

        if ((!packet_v1 && !packet_v2) ||
            ind.asdu[0] != RF_PACKET_MAGIC0 ||
            ind.asdu[1] != RF_PACKET_MAGIC1) {
            reject_reason = RF_REJECT_MALFORMED;
            return true;
        }

        report_seq = ind.asdu[4];
        if (ind.asdu[3] == RF_CMD_REPORT) {
            reported_power = (int8_t)ind.asdu[5];
            power_report = true;
            if (packet_v2) {
                reported_mode = ind.asdu[6];
                if ((reported_mode != RF_MODE_AUTO &&
                     reported_mode != RF_MODE_MANUAL) ||
                    ind.asdu[7] != 0) {
                    reject_reason = RF_REJECT_MALFORMED;
                    return true;
                }
            }
        } else {
            reject_reason = RF_REJECT_UNEXPECTED_CMD;
            return true;
        }
    }

    recent_rssi = esp_ieee802154_get_recent_rssi();
    int frame_lqi = ind.lqi;
    bool active_child = rf_power_known_active_child(ind.src_short_addr);
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    taskENTER_CRITICAL(&s_rf_power_lock);
    (void)rf_power_expire_waiting_if_due_locked(now);
    if (active_child) {
        s_rf_node_addr = ind.src_short_addr;
        s_rf_last_rx_ms = now;
    }

    if (power_report) {
        expected_seq = s_rf_expected_report_seq;
        expected_mode = s_rf_expected_report_mode;
        expected_power = s_rf_expected_report_power;
        expected_user = s_rf_expected_user_transaction;
        last_seq = s_rf_last_report_seq;
        old_power_unknown = s_rf_node_power_dbm == RF_POWER_UNKNOWN_DBM;

        if (!active_child) {
            reject_reason = RF_REJECT_UNKNOWN_CHILD;
        } else if (!rf_power_is_allowed_value(reported_power)) {
            reject_reason = RF_REJECT_INVALID_POWER;
        } else if (s_rf_report_waiting) {
            if (packet_v2 &&
                report_seq == expected_seq &&
                (expected_power == RF_POWER_UNKNOWN_DBM ||
                 reported_power == expected_power) &&
                (expected_mode == RF_MODE_UNKNOWN ||
                 reported_mode == expected_mode)) {
                accept_report = true;
                matched_transaction = expected_user;
                strlcpy(response_request_id, s_rf_expected_request_id,
                        sizeof(response_request_id));
                s_rf_expected_request_id[0] = '\0';
                s_rf_report_waiting = false;
                s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
                s_rf_expected_report_mode = RF_MODE_UNKNOWN;
                s_rf_wait_started_ms = 0;
            } else {
                reject_reason = RF_REJECT_MISMATCH;
            }
        } else if (rf_power_report_is_new_unsolicited_locked(report_seq)) {
            accept_report = true;
        } else {
            reject_reason = RF_REJECT_OLD;
        }
    }

    if (accept_report) {
        s_rf_node_power_dbm = reported_power;
        s_rf_node_mode = packet_v2 ? reported_mode : RF_MODE_UNKNOWN;
        if (!s_rf_policy_user_transaction && packet_v2 &&
            ((s_rf_mode == RF_MODE_MANUAL &&
              reported_mode == RF_MODE_MANUAL &&
              reported_power == s_rf_manual_power_dbm) ||
             (s_rf_mode == RF_MODE_AUTO &&
              reported_mode == RF_MODE_AUTO))) {
            s_rf_policy_apply_pending = false;
            s_rf_policy_request_id[0] = '\0';
        }
        if (s_rf_policy_apply_pending) {
            s_rf_last_eval_ms = 0;
        }
        s_rf_last_report_seq = report_seq;
        s_rf_last_report_seq_valid = true;
        s_rf_power_confirmed_ms = now;
        // Discard old-power history. The encrypted report itself is the first
        // sample at the new power; subsequent standard frames refine the EMA.
        s_rf_lqi_ema = frame_lqi;
        s_rf_lqi_samples = 1;
        s_rf_lqi_low_windows = 0;
        s_rf_lqi_high_windows = 0;
    } else if (!is_private && active_child && frame_lqi >= 0) {
        if (s_rf_lqi_ema < 0) {
            s_rf_lqi_ema = frame_lqi;
        } else {
            s_rf_lqi_ema = (s_rf_lqi_ema * 3 + frame_lqi) / 4;
        }
        if (s_rf_lqi_samples < UINT8_MAX) {
            s_rf_lqi_samples++;
        }
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    // Log only after leaving the restricted critical section.
    if (active_child) {
        ESP_LOGI(TAG, "RF sample cluster=0x%04x lqi=%d recent_rssi=%d",
                 (unsigned)ind.cluster_id, frame_lqi, (int)recent_rssi);
    }
    if (accept_report) {
        ESP_LOGI(TAG,
                 "RF power: report accepted from %s to %s/%d dBm seq=%u",
                 old_power_unknown ? "unknown" : "previous",
                 rf_power_mode_name(reported_mode),
                 (int)reported_power, report_seq);
        rf_power_publish_state(ind.src_short_addr, reported_power,
                                reported_mode,
                                matched_transaction ? "confirmed"
                                                   : "report_received",
                                matched_transaction ? response_request_id
                                                   : NULL);
    } else if (reject_reason == RF_REJECT_UNSECURED) {
        ESP_LOGW(TAG, "RF power: reject unsecured frame cluster=0x%04x",
                 (unsigned)ind.cluster_id);
    } else if (reject_reason == RF_REJECT_MALFORMED) {
        ESP_LOGW(TAG, "RF power: reject malformed private frame length=%lu",
                 (unsigned long)ind.asdu_length);
    } else if (reject_reason == RF_REJECT_UNEXPECTED_CMD) {
        ESP_LOGW(TAG, "RF power: reject unexpected private command");
    } else if (reject_reason == RF_REJECT_UNKNOWN_CHILD) {
        ESP_LOGW(TAG,
                 "RF power: reject report from unknown/inactive child 0x%04x",
                 (unsigned)ind.src_short_addr);
    } else if (reject_reason == RF_REJECT_INVALID_POWER) {
        ESP_LOGW(TAG, "RF power: reject invalid reported power=%d",
                 (int)reported_power);
    } else if (reject_reason == RF_REJECT_MISMATCH) {
        ESP_LOGW(TAG,
                 "RF power: reject mismatched report seq=%u expected=%u power=%d expected=%d mode=%s expected=%s",
                 report_seq, expected_seq,
                 (int)reported_power, (int)expected_power,
                 rf_power_mode_name(reported_mode),
                 rf_power_mode_name(expected_mode));
    } else if (reject_reason == RF_REJECT_OLD) {
        ESP_LOGW(TAG,
                 "RF power: reject duplicate/old report seq=%u last=%u",
                 report_seq, last_seq);
    }

    return is_private;
}

static esp_err_t rf_power_send_to_node(uint8_t command, int8_t power_dbm,
                                       uint8_t mode, uint8_t *sent_seq)
{
    uint8_t seq = 0;
    uint16_t addr = 0;

    if (mode != RF_MODE_AUTO && mode != RF_MODE_MANUAL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    addr = s_rf_node_addr;
    seq = ++s_rf_packet_sequence;
    if (seq == 0 || seq >= 0x80) {
        seq = 1;
        s_rf_packet_sequence = 1;
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    uint8_t asdu[RF_PACKET_LEN] = {
        RF_PACKET_MAGIC0,
        RF_PACKET_MAGIC1,
        RF_PACKET_VERSION,
        command,
        seq,
        (uint8_t)power_dbm,
        mode,
        0,
    };
    esp_zb_apsde_data_req_t req = {
        .dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .dst_addr.addr_short = addr,
        .dst_endpoint = RF_ZB_ENDPOINT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = RF_POWER_CLUSTER_ID,
        .src_endpoint = RF_ZB_ENDPOINT,
        .asdu_length = sizeof(asdu),
        .asdu = asdu,
        .tx_options = ESP_ZB_APSDE_TX_OPT_SECURITY_ENABLED | ESP_ZB_APSDE_TX_OPT_ACK_TX,
        .use_alias = false,
        .alias_src_addr = 0,
        .alias_seq_num = 0,
        .radius = 10,
    };
    esp_err_t err = esp_zb_aps_data_request(&req);
    ESP_LOGI(TAG, "RF power -> node=0x%04x cmd=%u power=%d mode=%s seq=%u: %s",
             (unsigned)addr, command, power_dbm, rf_power_mode_name(mode),
             seq, esp_err_to_name(err));
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_expected_request_id[0] = '\0';
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
    if (err == ESP_OK && sent_seq != NULL) {
        *sent_seq = seq;
    }
    return err;
}

static void rf_power_remote_query(const char *request_id)
{
    uint16_t addr = 0;
    bool report_waiting = false;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    taskENTER_CRITICAL(&s_rf_power_lock);
    addr = s_rf_node_addr;
    report_waiting = s_rf_report_waiting;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (addr == 0) {
        ESP_LOGW(TAG, "RF power: QUERY requested before node became active");
        return;
    }
    if (report_waiting) {
        ESP_LOGW(TAG, "RF power: QUERY ignored while another report is pending");
        return;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_expected_user_transaction = true;
    rf_power_set_request_id_locked(request_id);
    taskEXIT_CRITICAL(&s_rf_power_lock);
    request_id = s_rf_expected_request_id[0] != '\0' ? s_rf_expected_request_id : NULL;

    uint8_t sent_seq = 0;
    rf_power_publish_querying(addr, request_id);
    if (rf_power_send_to_node(RF_CMD_QUERY, 0, RF_MODE_AUTO, &sent_seq) == ESP_OK) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
        s_rf_expected_report_mode = RF_MODE_UNKNOWN;
        s_rf_expected_user_transaction = true;
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    } else {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_expected_user_transaction = false;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}

static void rf_power_remote_set_manual(int8_t target_power_dbm, const char *request_id)
{
    uint16_t addr = 0;
    int8_t current_power_dbm = RF_POWER_UNKNOWN_DBM;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    if (!rf_power_is_allowed_value(target_power_dbm)) {
        ESP_LOGW(TAG, "RF power: reject remote target=%d dBm", (int)target_power_dbm);
        return;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_mode = RF_MODE_MANUAL;
    s_rf_manual_power_dbm = target_power_dbm;
    s_rf_policy_apply_pending = true;
    s_rf_report_waiting = false;
    s_rf_expected_report_seq = 0;
    s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
    s_rf_expected_report_mode = RF_MODE_UNKNOWN;
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    s_rf_wait_started_ms = 0;
    s_rf_last_eval_ms = 0;
    s_rf_policy_user_transaction = true;
    rf_set_policy_request_id_locked(request_id);
    addr = s_rf_node_addr;
    current_power_dbm = s_rf_node_power_dbm;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_policy_persist(RF_MODE_MANUAL, target_power_dbm);

    if (addr == 0) {
        ESP_LOGI(TAG, "RF power: manual policy saved; apply when node joins");
        return;
    }
    if (current_power_dbm == RF_POWER_UNKNOWN_DBM) {
        uint8_t sent_seq = 0;
        if (rf_power_send_to_node(RF_CMD_QUERY, 0, RF_MODE_AUTO, &sent_seq) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
            s_rf_expected_report_mode = RF_MODE_UNKNOWN;
            s_rf_expected_user_transaction = false;
            s_rf_expected_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }

    uint8_t sent_seq = 0;
    if (rf_power_send_to_node(RF_CMD_SET, target_power_dbm, RF_MODE_MANUAL, &sent_seq) == ESP_OK) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = target_power_dbm;
        s_rf_expected_report_mode = RF_MODE_MANUAL;
        s_rf_expected_user_transaction = s_rf_policy_user_transaction;
        strlcpy(s_rf_expected_request_id, s_rf_policy_request_id, sizeof(s_rf_expected_request_id));
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}

static void rf_power_remote_set_auto(const char *request_id)
{
    uint16_t addr = 0;
    int8_t current_power_dbm = RF_POWER_UNKNOWN_DBM;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_mode = RF_MODE_AUTO;
    s_rf_policy_apply_pending = true;
    s_rf_report_waiting = false;
    s_rf_expected_report_seq = 0;
    s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
    s_rf_expected_report_mode = RF_MODE_UNKNOWN;
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    s_rf_wait_started_ms = 0;
    s_rf_last_eval_ms = 0;
    s_rf_policy_user_transaction = true;
    rf_set_policy_request_id_locked(request_id);
    addr = s_rf_node_addr;
    current_power_dbm = s_rf_node_power_dbm;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_policy_persist(RF_MODE_AUTO, s_rf_manual_power_dbm);
    ESP_LOGI(TAG, "RF power: automatic policy enabled");

    if (addr == 0) {
        ESP_LOGI(TAG, "RF power: automatic policy saved; apply when node joins");
        return;
    }
    if (current_power_dbm == RF_POWER_UNKNOWN_DBM) {
        uint8_t sent_seq = 0;
        if (rf_power_send_to_node(RF_CMD_QUERY, 0, RF_MODE_AUTO, &sent_seq) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
            s_rf_expected_report_mode = RF_MODE_UNKNOWN;
            s_rf_expected_user_transaction = false;
            s_rf_expected_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }

    uint8_t sent_seq = 0;
    if (rf_power_send_to_node(RF_CMD_SET, current_power_dbm, RF_MODE_AUTO, &sent_seq) == ESP_OK) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = current_power_dbm;
        s_rf_expected_report_mode = RF_MODE_AUTO;
        s_rf_expected_user_transaction = s_rf_policy_user_transaction;
        strlcpy(s_rf_expected_request_id, s_rf_policy_request_id, sizeof(s_rf_expected_request_id));
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}

static void rf_power_controller_periodic(void)
{
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    uint16_t addr = 0;
    uint32_t last_rx = 0;
    int link_lqi = -1;
    uint8_t samples = 0;
    int8_t current_power = 0;
    int8_t target_power = 0;
    uint8_t mode = RF_MODE_AUTO;
    uint8_t node_mode = RF_MODE_UNKNOWN;
    int8_t manual_power = RF_POWER_AUTO_MIN_DBM;
    bool policy_pending = false;
    bool policy_user = false;
    char policy_request_id[17] = {0};
    bool report_waiting = false;
    uint32_t wait_started_ms = 0;
    uint32_t confirmed_ms = 0;

    if (now - s_rf_last_eval_ms < RF_POWER_EVAL_MS) {
        return;
    }
    s_rf_last_eval_ms = now;

    taskENTER_CRITICAL(&s_rf_power_lock);
    addr = s_rf_node_addr;
    last_rx = s_rf_last_rx_ms;
    link_lqi = s_rf_lqi_ema;
    samples = s_rf_lqi_samples;
    current_power = s_rf_node_power_dbm;
    node_mode = s_rf_node_mode;
    mode = s_rf_mode;
    manual_power = s_rf_manual_power_dbm;
    policy_pending = s_rf_policy_apply_pending;
    policy_user = s_rf_policy_user_transaction;
    strlcpy(policy_request_id, s_rf_policy_request_id,
            sizeof(policy_request_id));
    report_waiting = s_rf_report_waiting;
    wait_started_ms = s_rf_wait_started_ms;
    confirmed_ms = s_rf_power_confirmed_ms;
    s_rf_lqi_samples = 0;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (addr == 0) {
        return;
    }
    if (now - last_rx > RF_POWER_FRESH_MS) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_lqi_ema = -1;
        s_rf_lqi_low_windows = 0;
        s_rf_lqi_high_windows = 0;
        s_rf_node_mode = RF_MODE_UNKNOWN;
        s_rf_report_waiting = false;
        s_rf_expected_report_seq = 0;
        s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
        s_rf_expected_report_mode = RF_MODE_UNKNOWN;
        s_rf_expected_user_transaction = false;
        s_rf_expected_request_id[0] = '\0';
        s_rf_wait_started_ms = 0;
        s_rf_policy_apply_pending = true;
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }
    bool wait_timed_out = report_waiting &&
                          now - wait_started_ms >= RF_POWER_SETTLE_MS;
    if (current_power == RF_POWER_UNKNOWN_DBM || wait_timed_out) {
        uint8_t sent_seq = 0;
        if (rf_power_send_to_node(RF_CMD_QUERY, 0, RF_MODE_AUTO,
                                  &sent_seq) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
            s_rf_expected_report_mode = RF_MODE_UNKNOWN;
            s_rf_expected_user_transaction = false;
            s_rf_expected_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }
    if (!report_waiting && confirmed_ms != 0 &&
        now - confirmed_ms >= RF_POWER_RECONCILE_MS) {
        uint8_t sent_seq = 0;
        ESP_LOGI(TAG, "RF power: periodic reconcile current=%d dBm", current_power);
        if (rf_power_send_to_node(RF_CMD_QUERY, 0, RF_MODE_AUTO,
                                  &sent_seq) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
            s_rf_expected_report_mode = RF_MODE_UNKNOWN;
            s_rf_expected_user_transaction = false;
            s_rf_expected_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }
    if (report_waiting) {
        ESP_LOGI(TAG, "RF power: waiting for report seq=%u current=%d dBm",
                 s_rf_expected_report_seq, current_power);
        return;
    }

    if (mode == RF_MODE_MANUAL) {
        if (node_mode == RF_MODE_MANUAL &&
            current_power == manual_power) {
            bool already_confirmed = false;
            char response_request_id[17] = {0};

            if (policy_pending) {
                taskENTER_CRITICAL(&s_rf_power_lock);
                bool state_still_matches =
                    s_rf_policy_apply_pending &&
                    s_rf_policy_user_transaction == policy_user &&
                    s_rf_mode == RF_MODE_MANUAL &&
                    s_rf_node_addr == addr &&
                    s_rf_node_mode == RF_MODE_MANUAL &&
                    s_rf_manual_power_dbm == manual_power &&
                    s_rf_node_power_dbm == manual_power;

                if (policy_user) {
                    state_still_matches = state_still_matches &&
                        rf_power_request_id_valid(policy_request_id) &&
                        strcmp(s_rf_policy_request_id,
                               policy_request_id) == 0;
                } else {
                    state_still_matches = state_still_matches &&
                        s_rf_policy_request_id[0] == '\0';
                }

                // Do not consume a newer transaction if this periodic snapshot
                // was overtaken before publication.
                if (state_still_matches) {
                    if (policy_user) {
                        strlcpy(response_request_id, policy_request_id,
                                sizeof(response_request_id));
                        already_confirmed = true;
                    }
                    s_rf_policy_apply_pending = false;
                    s_rf_policy_user_transaction = false;
                    s_rf_policy_request_id[0] = '\0';
                }
                taskEXIT_CRITICAL(&s_rf_power_lock);
            }
            if (already_confirmed) {
                rf_power_publish_state(addr, manual_power, RF_MODE_MANUAL,
                                       "confirmed", response_request_id);
            }
            return;
        }
        ESP_LOGW(TAG,
                 "RF power: manual policy mismatch actual=%s/%d dBm desired=%d dBm; reconcile",
                 rf_power_mode_name(node_mode), (int)current_power,
                 (int)manual_power);
        policy_pending = true;
    } else if (mode == RF_MODE_AUTO && node_mode == RF_MODE_MANUAL) {
        ESP_LOGW(TAG,
                 "RF power: node is manual but desired is automatic; reconcile");
        policy_pending = true;
    }

    if (policy_pending) {
        uint8_t sent_seq = 0;

        if (mode == RF_MODE_MANUAL) {
            if (current_power == manual_power &&
                node_mode == RF_MODE_MANUAL && !policy_user) {
                taskENTER_CRITICAL(&s_rf_power_lock);
                s_rf_policy_apply_pending = false;
                s_rf_policy_user_transaction = false;
                s_rf_policy_request_id[0] = '\0';
                taskEXIT_CRITICAL(&s_rf_power_lock);
                return;
            }
            target_power = manual_power;
            if (rf_power_send_to_node(RF_CMD_SET, target_power,
                                      RF_MODE_MANUAL,
                                      &sent_seq) == ESP_OK) {
                taskENTER_CRITICAL(&s_rf_power_lock);
                s_rf_report_waiting = true;
                s_rf_expected_report_seq = sent_seq;
                s_rf_expected_report_power = target_power;
                s_rf_expected_report_mode = RF_MODE_MANUAL;
                s_rf_expected_user_transaction = policy_user;
                strlcpy(s_rf_expected_request_id, s_rf_policy_request_id, sizeof(s_rf_expected_request_id));
                s_rf_policy_user_transaction = false;
                s_rf_policy_request_id[0] = '\0';
                s_rf_wait_started_ms = now;
                taskEXIT_CRITICAL(&s_rf_power_lock);
            }
            return;
        }

        if (node_mode == RF_MODE_AUTO && !policy_user) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_policy_apply_pending = false;
            s_rf_policy_user_transaction = false;
            s_rf_policy_request_id[0] = '\0';
            taskEXIT_CRITICAL(&s_rf_power_lock);
            return;
        }

        target_power = current_power;
        if (rf_power_send_to_node(RF_CMD_SET, target_power,
                                  RF_MODE_AUTO,
                                  &sent_seq) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = target_power;
            s_rf_expected_report_mode = RF_MODE_AUTO;
            s_rf_expected_user_transaction = policy_user;
            strlcpy(s_rf_expected_request_id, s_rf_policy_request_id, sizeof(s_rf_expected_request_id));
            s_rf_policy_user_transaction = false;
            s_rf_policy_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }

    if (samples < RF_POWER_SAMPLES_NEEDED) {
        ESP_LOGI(TAG, "RF power: wait samples=%u/%u lqi_ema=%d",
                 samples, RF_POWER_SAMPLES_NEEDED, link_lqi);
        return;
    }

    uint8_t current_index = rf_power_nearest_index(current_power);
    uint8_t min_index = rf_power_min_index();
    uint8_t max_index = rf_power_max_index();
    uint8_t target_index = current_index;

    if (current_index < min_index || current_index > max_index) {
        ESP_LOGW(TAG, "RF power: current=%d dBm outside configured policy",
                 (int)current_power);
        return;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_lqi_low_windows = 0;
    if (link_lqi > RF_LQI_LOWER) {
        if (s_rf_lqi_high_windows < UINT8_MAX) {
            s_rf_lqi_high_windows++;
        }
    } else {
        s_rf_lqi_high_windows = 0;
    }

    if (s_rf_lqi_high_windows >= RF_LQI_LOWER_WINDOWS &&
        current_index > min_index) {
        target_index = current_index - 1;
        s_rf_lqi_low_windows = 0;
        s_rf_lqi_high_windows = 0;
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (target_index == current_index) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        uint8_t low_windows = s_rf_lqi_low_windows;
        uint8_t high_windows = s_rf_lqi_high_windows;
        taskEXIT_CRITICAL(&s_rf_power_lock);
        ESP_LOGI(TAG,
                 "RF power: keep %d dBm lqi_ema=%d low_windows=%u high_windows=%u",
                 current_power, link_lqi, low_windows, high_windows);
        return;
    }

    target_power = s_rf_power_table[target_index];
    uint8_t sent_seq = 0;
    if (rf_power_send_to_node(RF_CMD_SET, target_power, RF_MODE_AUTO,
                              &sent_seq) == ESP_OK) {
        // Queue acceptance is not proof. Accept only the node's encrypted
        // REPORT carrying this request sequence, requested power and mode.
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = target_power;
        s_rf_expected_report_mode = RF_MODE_AUTO;
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}

// ==================== OLED SSD1306 ====================
// Minimal local text driver for this 128x64 yellow/blue SSD1306 panel.
// The installed espressif/ssd1306 1.0.5 text/font path produced garbled glyphs
// on this board.  Use a standard 5x7 font, page-order framebuffer, horizontal
// addressing, and short retried I2C strips with local bus/panel recovery.

#define OLED_FB_SIZE            (OLED_WIDTH * OLED_HEIGHT / 8)
#define OLED_I2C_FREQ_HZ        100000
#define OLED_I2C_TIMEOUT_MS     200
#define OLED_CHUNK_COLS         32
#define OLED_TX_ATTEMPTS        3
#define OLED_BUS_RECOVERY_PULSES 9

static uint8_t s_fb[OLED_FB_SIZE];
static uint32_t s_oled_tx_retry_count;
static uint32_t s_oled_tx_failure_count;
static uint32_t s_oled_recovery_count;
static uint32_t s_oled_consecutive_flush_failures;

static esp_err_t oled_i2c_force_bus_idle(void)
{
    // If a transfer was interrupted and the SSD1306 holds SDA low, temporarily
    // bit-bang nine SCL pulses followed by STOP, then return the pins to I2C.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << I2C_SDA_PIN) | (1ULL << I2C_SCL_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_set_level(I2C_SDA_PIN, 1);
    gpio_set_level(I2C_SCL_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    for (int i = 0; i < OLED_BUS_RECOVERY_PULSES; i++) {
        gpio_set_level(I2C_SCL_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(I2C_SCL_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    gpio_set_level(I2C_SDA_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(I2C_SCL_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(I2C_SDA_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    gpio_reset_pin(I2C_SDA_PIN);
    gpio_reset_pin(I2C_SCL_PIN);
    return ESP_OK;
}

static esp_err_t oled_i2c_init(void)
{
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = OLED_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_NUM_0, &i2c_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return ret;
}

static esp_err_t oled_i2c_write_once(uint8_t control, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(h);
    i2c_master_write_byte(h, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, control, true);
    if (data != NULL && len > 0) {
        i2c_master_write(h, data, len, true);
    }
    i2c_master_stop(h);

    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, h,
                                         pdMS_TO_TICKS(OLED_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(h);
    return ret;
}

static esp_err_t oled_i2c_write(uint8_t control, const uint8_t *data, size_t len)
{
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 0; attempt < OLED_TX_ATTEMPTS; attempt++) {
        ret = oled_i2c_write_once(control, data, len);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        s_oled_tx_retry_count++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_oled_tx_failure_count++;
    return ret;
}

static bool oled_check_device(void)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(OLED_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK);
}

static bool oled_init(void)
{
    // Standard SSD1306 128x64 initialization. A1+C8 is the previously
    // verified orientation for this particular yellow/blue module.
    static const uint8_t init_cmds[] = {
        0xAE,             // display off
        0xD5, 0x80,       // clock divide / oscillator
        0xA8, 0x3F,       // multiplex ratio 1/64
        0xD3, 0x00,       // display offset 0
        0x40,             // display start line 0
        0xA1,             // segment remap
        0xC8,             // COM scan direction (upright on this panel)
        0xDA, 0x12,       // COM pins: 128x64 alternative mapping
        0x81, 0xCF,       // contrast
        0xD9, 0xF1,       // pre-charge period
        0xDB, 0x40,       // VCOMH deselect level
        0x8D, 0x14,       // enable charge pump
        0xA4,             // output follows GDDRAM
        0xA6,             // normal display (not inverse)
        0x20, 0x00,       // horizontal addressing mode
        0x21, 0x00, 0x7F, // column window 0..127
        0x22, 0x00, 0x07, // page window 0..7
        0xAF,             // display on
    };

    esp_err_t ret = oled_i2c_write(0x00, init_cmds, sizeof(init_cmds));
    memset(s_fb, 0, sizeof(s_fb));
    return (ret == ESP_OK);
}

static bool oled_recover_bus_and_panel(void)
{
    s_oled_recovery_count++;
    ESP_LOGW(TAG, "OLED refresh failed; local recovery #%lu (retries=%lu failures=%lu)",
             (unsigned long)s_oled_recovery_count,
             (unsigned long)s_oled_tx_retry_count,
             (unsigned long)s_oled_tx_failure_count);

    // Keep Zigbee, Wi-Fi and MQTT alive; only rebuild the I2C bus/display.
    i2c_driver_delete(I2C_NUM_0);
    vTaskDelay(pdMS_TO_TICKS(80));
    (void)oled_i2c_force_bus_idle();

    if (oled_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "OLED I2C driver recovery failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(80));

    if (!oled_check_device()) {
        ESP_LOGW(TAG, "OLED did not ACK after I2C bus recovery");
        return false;
    }
    if (!oled_init()) {
        ESP_LOGW(TAG, "OLED SSD1306 reinitialization failed");
        return false;
    }

    ESP_LOGI(TAG, "OLED recovered by I2C/SSD1306 local reset");
    return true;
}

static bool oled_flush_once(void)
{
    // Refresh in short horizontal strips. Every strip sets an explicit
    // window, so a retry cannot continue at the wrong GDDRAM location and
    // turn later columns into horizontal garbage.
    for (int page = 0; page < OLED_HEIGHT / 8; page++) {
        for (int x0 = 0; x0 < OLED_WIDTH; x0 += OLED_CHUNK_COLS) {
            int x1 = x0 + OLED_CHUNK_COLS - 1;
            uint8_t window_cmds[] = {
                0x20, 0x00,                     // horizontal addressing
                0x21, (uint8_t)x0, (uint8_t)x1,
                0x22, (uint8_t)page, (uint8_t)page,
            };

            esp_err_t ret = oled_i2c_write(0x00, window_cmds, sizeof(window_cmds));
            if (ret != ESP_OK) {
                return false;
            }

            const uint8_t *chunk = &s_fb[page * OLED_WIDTH + x0];
            ret = oled_i2c_write(0x40, chunk, OLED_CHUNK_COLS);
            if (ret != ESP_OK) {
                return false;
            }
        }
    }

    return true;
}

static bool oled_flush(void)
{
    if (oled_flush_once()) {
        if (s_oled_consecutive_flush_failures > 0) {
            ESP_LOGI(TAG, "OLED refresh recovered after %lu failed frame(s)",
                     (unsigned long)s_oled_consecutive_flush_failures);
        }
        s_oled_consecutive_flush_failures = 0;
        return true;
    }

    s_oled_consecutive_flush_failures++;
    if (!oled_recover_bus_and_panel()) {
        return false;
    }

    // s_fb is preserved, so the next task tick redraws the complete screen.
    return oled_flush_once();
}
static void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

// Compact 5x7 ASCII font, space (0x20) through Z (0x5A).
static const uint8_t FONT_5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x01,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x03,0x04,0x78,0x04,0x03}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
};

static void oled_char(int x, int y, char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c == '_') {
        c = '-';
    }
    if (c < ' ' || c > 'Z') {
        return;
    }

    int idx = c - ' ';
    for (int col = 0; col < 5; col++) {
        uint8_t line = FONT_5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            int px = x + col;
            int py = y + row;
            if ((line & (1 << row)) && px >= 0 && px < OLED_WIDTH && py >= 0 && py < OLED_HEIGHT) {
                s_fb[(py / 8) * OLED_WIDTH + px] |= (uint8_t)(1u << (py % 8));
            }
        }
    }
}

static void oled_text(int x, int y, const char *s)
{
    while (*s != 0) {
        oled_char(x, y, *s);
        x += 6;
        s++;
    }
}

// ==================== Zigbee ====================

#define MAX_CHILDREN                10
#define INSTALLCODE_POLICY_ENABLE   false
#define ESP_ZB_CHANNEL_MASK         (1l << 26)  // Channel 26
#define ZB_ONLY_RF_DIAG             0  // Temporary RF/coexistence diagnostic
// Wi-Fi now coexists safely with Zigbee (dedicated stack loop and RF
// priority fixes landed earlier). Waiting for a node before bringing up
// Wi-Fi used to delay cloud reconnection by 3-5 minutes whenever no node
// was present, so the commissioning-first path is disabled.
#define ZB_JOIN_BEFORE_WIFI         0  // Bring Wi-Fi/MQTT up in parallel with Zigbee
#define ZB_JOIN_FIRST_TIMEOUT_MS    180000
#define ZB_REJOIN_FIRST_TIMEOUT_MS 300000

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
            // Formation is driven by our own NVS flag: after a full flash
            // erase some ZBOSS builds signal DEVICE_REBOOT on blank NVRAM and
            // skip formation, leaving the coordinator beaconless (nodes saw
            // "Can't find PAN"). Force formation until we have persisted one.
            if (s_network_formed) {
                ESP_LOGI(TAG, "Zigbee: Rebooted, open network");
                ESP_LOGI(TAG, "Zigbee: PANID=0x%04x channel=%d",
                         (unsigned)esp_zb_get_pan_id(),
                         (int)esp_zb_get_current_channel());
                esp_zb_bdb_open_network(180);
            } else {
                ESP_LOGI(TAG, "Zigbee: Start network formation (flag absent)");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            }
        } else {
            ESP_LOGE(TAG, "Zigbee: Failed to start (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee: Network formed!");
            ESP_LOGI(TAG, "Zigbee: PANID=0x%04x channel=%d mask=0x%lx",
                     (unsigned)esp_zb_get_pan_id(),
                     (int)esp_zb_get_current_channel(),
                     (unsigned long)esp_zb_get_channel_mask());
            s_zigbee_ok = true;
            s_network_formed = true;
            nvs_handle_t nvh;
            if (nvs_open("gwzb", NVS_READWRITE, &nvh) == ESP_OK) {
                nvs_set_u8(nvh, "formed", 1);
                nvs_commit(nvh);
                nvs_close(nvh);
            }
            esp_zb_bdb_open_network(180);
        } else {
            ESP_LOGE(TAG, "Zigbee: Formation failed, retrying...");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        {
            // This signal has been observed arriving in a non-preemptible ZBOSS
            // context. Copy it and defer logging/MQTT to a regular task.
            esp_zb_zdo_signal_device_annce_params_t *params =
                (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (params != NULL) {
                zb_app_event_t event = {
                    .type = ZB_APP_EVENT_JOIN,
                    .addr = params->device_short_addr,
                };
                memcpy(event.ieee, params->ieee_addr, sizeof(event.ieee));
                zb_app_event_send(&event);
            }
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION:
        {
            // Like device announce, defer table mutation/logging/MQTT out of ZBOSS context.
            esp_zb_zdo_signal_leave_indication_params_t *params =
                (esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (params != NULL) {
                zb_app_event_t event = {
                    .type = ZB_APP_EVENT_LEAVE,
                };
                memcpy(event.ieee, params->device_addr, sizeof(event.ieee));
                zb_app_event_send(&event);
            }
        }
        break;
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK) {
            const uint8_t *permit_duration =
                (const uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (permit_duration != NULL) {
                zb_app_signal_event_send(sig_type, err_status, *permit_duration);
            }
        } else {
            zb_app_signal_event_send(sig_type, err_status, 0);
        }
        break;
    case ESP_ZB_SE_SIGNAL_REJOIN:
    case ESP_ZB_SE_SIGNAL_CHILD_REJOIN:
    case ESP_ZB_BDB_SIGNAL_TC_REJOIN_DONE:
    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        zb_app_signal_event_send(sig_type, err_status, 0);
        break;
    case ESP_ZB_NLME_STATUS_INDICATION:
        {
            // The real NWK status code is in the signal payload, not in
            // err_status. Rate-limit all non-zero indications globally.
            const gw_zb_nlme_status_signal_params_t *nlme_params =
                (const gw_zb_nlme_status_signal_params_t *)
                    esp_zb_app_signal_get_params(p_sg_p);
            if (nlme_params != NULL &&
                nlme_params->nlme_status.status != 0x00 &&
                zb_nlme_diagnostic_allowed()) {
                uint8_t nlme_status = nlme_params->nlme_status.status;
                uint16_t network_addr =
                    (uint16_t)nlme_params->nlme_status.network_addr_le[0] |
                    ((uint16_t)nlme_params->nlme_status.network_addr_le[1] << 8);
                zb_app_event_t event = {
                    .type = ZB_APP_EVENT_SIGNAL,
                    .signal = (uint8_t)sig_type,
                    .status = (int32_t)err_status,
                    .value = nlme_status,
                    .addr = network_addr,
                    .raw = nlme_params->nlme_status.unknown_command_id,
                };
                (void)zb_signal_event_send(&event);
            }
        }
        break;
    default:
        ESP_LOGI(TAG, "Zigbee signal: %d", sig_type);
        break;
    }
}

// ==================== ZCL sensor data aggregation ====================

#define EP_GW                       10      // gateway endpoint (matches sensor nodes)
#define MAX_ZB_NODES                10
#define ZB_MASK_TEMP                0x01
#define ZB_MASK_HUM                 0x02
#define ZB_MASK_LUX                 0x04
#define ZB_MASK_ALL                 (ZB_MASK_TEMP | ZB_MASK_HUM | ZB_MASK_LUX)
#define ZB_TEMP_INVALID             ((int16_t)0x8000)
#define ZB_VALUE_INVALID            0xFFFF
#define ZB_REPORT_PARTIAL_DELAY_MS  30000
#define ZB_TELEMETRY_DEDUP_MS       9000

typedef struct {
    uint16_t addr;          // 0 = free slot
    int16_t  temp_raw;      // 0.01 C
    uint16_t hum_raw;       // 0.01 %
    uint16_t lux_raw;       // Zigbee log scale: 10000 * log10(lux + 1)
    uint8_t  ieee[8];       // extended address, used for retained status
    uint8_t  valid;         // fields ever received
    uint8_t  dirty;         // fields updated since last MQTT publish
    uint8_t  active;        // currently joined child
    uint8_t  onoff_valid;   // On/Off state has been received
    uint8_t  onoff_value;   // latest On/Off state (0=off, 1=on)
    uint32_t first_dirty_ms;
    uint32_t last_report_ms;
} zb_node_t;

static zb_node_t s_zb_nodes[MAX_ZB_NODES];
static portMUX_TYPE s_zb_lock = portMUX_INITIALIZER_UNLOCKED;

// Verify the report source independently from the short address in the frame.
// This helper uses only s_zb_lock and is intentionally called before taking
// s_rf_power_lock to avoid a lock nesting/deadlock.
static bool rf_power_known_active_child(uint16_t addr)
{
    bool found = false;

    portENTER_CRITICAL(&s_zb_lock);
    for (uint8_t i = 0; i < MAX_ZB_NODES; ++i) {
        if (s_zb_nodes[i].addr == addr && s_zb_nodes[i].active) {
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_zb_lock);
    return found;
}

static zb_node_t *zb_node_get_or_create(uint16_t addr)
{
    zb_node_t *free_slot = NULL;
    for (int i = 0; i < MAX_ZB_NODES; i++) {
        if (s_zb_nodes[i].addr == addr) return &s_zb_nodes[i];
        if (s_zb_nodes[i].addr == 0 && free_slot == NULL) free_slot = &s_zb_nodes[i];
    }
    if (free_slot) {
        free_slot->addr = addr;
    }
    return free_slot;
}

// Unified ZCL core callback. ZBOSS may invoke this with preemption disabled,
// so this function only validates and queues a small value. All logging, node
// table mutation, and MQTT work happen in zb_forward_task().
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id != ESP_ZB_CORE_REPORT_ATTR_CB_ID || message == NULL) {
        return ESP_OK;
    }

    const esp_zb_zcl_report_attr_message_t *m =
        (const esp_zb_zcl_report_attr_message_t *)message;
    if (m->status != ESP_ZB_ZCL_STATUS_SUCCESS || m->attribute.id != 0x0000 ||
        m->attribute.data.value == NULL) {
        return ESP_OK;
    }

    zb_app_event_t event = {
        .type = ZB_APP_EVENT_REPORT,
        .addr = m->src_address.u.short_addr,
        .cluster = m->cluster,
    };
    const void *v = m->attribute.data.value;

    switch (m->cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT: {
        int16_t raw = *(const int16_t *)v;
        if (raw == ZB_TEMP_INVALID) {
            return ESP_OK;
        }
        event.raw = (uint16_t)raw;
        break;
    }
    case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
    case ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT: {
        uint16_t raw = *(const uint16_t *)v;
        if (raw == ZB_VALUE_INVALID) {
            return ESP_OK;
        }
        event.raw = raw;
        break;
    }
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF: {
        if (m->attribute.data.size != 1) {
            return ESP_OK;
        }
        uint8_t state = *(const uint8_t *)v;
        if (state > 1) {
            return ESP_OK;
        }
        event.value = state;
        break;
    }
    default:
        return ESP_OK;
    }

    zb_app_event_send(&event);
    return ESP_OK;
}

// Gateway-side endpoint: measurement clusters as client roles
static esp_zb_ep_list_t *create_gateway_ep(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01,  // mains
    };
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_basic_cluster(clusters,
        esp_zb_basic_cluster_create(&basic_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(clusters,
        esp_zb_identify_cluster_create(&identify_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_cluster_list_add_temperature_meas_cluster(clusters,
        esp_zb_temperature_meas_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_humidity_meas_cluster(clusters,
        esp_zb_humidity_meas_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_illuminance_meas_cluster(clusters,
        esp_zb_illuminance_meas_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_on_off_cluster(clusters,
        esp_zb_on_off_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_custom_cluster(clusters,
        esp_zb_zcl_attr_list_create(RF_POWER_CLUSTER_ID),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = EP_GW,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
    return ep_list;
}

static void zigbee_task(void *arg)
{
#if ZB_JOIN_BEFORE_WIFI
    // Wi-Fi is intentionally off during the clean RF commissioning/rejoin window.
    vTaskDelay(pdMS_TO_TICKS(500));
#else
    ESP_LOGI(TAG, "Zigbee: Waiting 10s for WiFi to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(10000));
#endif

    ESP_LOGI(TAG, "Starting Zigbee coordinator...");
    
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,
        .nwk_cfg.zczr_cfg.max_children = MAX_CHILDREN,
    };
    
    esp_zb_init(&zb_cfg);
    esp_zb_set_primary_network_channel_set(ESP_ZB_CHANNEL_MASK);
    if (!s_network_formed) {
        // Re-formation after an explicit pairing-state reset preserves the
        // documented laboratory PAN ID. With clean ZBOSS NVRAM, the stack
        // regenerates extended identity/key material.
        esp_zb_set_pan_id(0x2b9e);
        ESP_LOGI(TAG, "Zigbee: fresh formation requested; request PANID=0x%04x channel=26", 0x2b9e);
    }
    esp_zb_device_register(create_gateway_ep());
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_aps_data_indication_handler_register(rf_power_aps_indication_cb);

    ESP_ERROR_CHECK(esp_zb_start(false));

    ESP_LOGI(TAG, "Zigbee stack started with ZCL reporting");

    // The dedicated stack loop is the entry point recommended by ESP-Zigbee
    // 1.6. Polling the deprecated one-shot iteration every 100 ms adds latency
    // to coordinator timers and child/rejoin responses under Wi-Fi traffic.
    esp_zb_stack_main_loop();
    vTaskDelete(NULL);
}

// ==================== WiFi ====================

// Boot-time WiFi watchdog counter.
//
// RTC_NOINIT survives esp_restart()/RST but is not a durable NVS value. A
// magic word distinguishes a valid warm-reboot count from random power-up
// RAM. This is intentional: a fresh power-on must get the full watchdog
// budget again, while a failed power-on must not reset-loop forever.
#define WIFI_BOOT_MAGIC                 0x57424f54u  // "WBOT"

RTC_NOINIT_ATTR static uint32_t s_wifi_boot_magic;
RTC_NOINIT_ATTR static int s_wifi_boot_reboots;

static int boot_restart_count_get(void)
{
    return (s_wifi_boot_magic == WIFI_BOOT_MAGIC) ? s_wifi_boot_reboots : 0;
}

static void boot_restart_count_set(int v)
{
    s_wifi_boot_magic = WIFI_BOOT_MAGIC;
    s_wifi_boot_reboots = v;
}

static int s_rf_empty_scans = 0;
static bool s_rf_environment_alive = false;

// A normal router outage still leaves many neighboring 2.4GHz APs visible. If
// an active scan sees zero APs repeatedly, the Wi-Fi/RF subsystem is wedged in
// the state observed on 2026-09-10: esp_restart()/RTS did not recover it, but
// removing USB power did. Deep sleep powers down the modem much harder than a
// software restart and then timer-wakes the gateway automatically.
static void wifi_rf_deep_sleep_recovery(void)
{
    ESP_LOGE(TAG, "RF health: no APs on %d full scans; deep sleep %.1fs to power-cycle RF",
             WIFI_RF_EMPTY_LIMIT, WIFI_RF_SLEEP_US / 1000000.0f);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_wifi_deinit();
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    esp_sleep_enable_timer_wakeup(WIFI_RF_SLEEP_US);
    esp_deep_sleep_start();
}

// Returns true when at least one surrounding AP is visible. A successful scan
// proves RF reception works even if the home router itself is temporarily off.
static bool wifi_rf_health_scan(void)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    // A blocked active scan cannot run while an association attempt owns the
    // STA state. Disconnect here only for the diagnostic; the reconnect loop
    // starts a normal connection immediately after this health check.
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RF health scan failed to start: %s", esp_err_to_name(err));
        return false;  // Scan failure is not evidence that RF is dead.
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RF health scan count failed: %s", esp_err_to_name(err));
        return false;
    }

    bool target_visible = false;
    if (ap_count > 0) {
        wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (aps != NULL) {
            uint16_t got = ap_count;
            err = esp_wifi_scan_get_ap_records(&got, aps);
            if (err == ESP_OK) {
                for (int i = 0; i < got; ++i) {
                    if (strcmp((const char *)aps[i].ssid, WIFI_SSID) == 0) {
                        target_visible = true;
                    }
                }
            } else {
                ESP_LOGW(TAG, "RF health scan records failed: %s", esp_err_to_name(err));
            }
            free(aps);
        } else {
            esp_wifi_clear_ap_list();
            ESP_LOGW(TAG, "RF health scan could not allocate %u records", ap_count);
        }
    }

    if (ap_count > 0) {
        s_rf_empty_scans = 0;
        s_rf_environment_alive = true;
        ESP_LOGI(TAG, "RF health scan OK: %u AP(s), target %s",
                 ap_count, target_visible ? "VISIBLE" : "not visible");
        return true;
    }

    s_rf_environment_alive = false;
    s_rf_empty_scans++;
    ESP_LOGW(TAG, "RF health scan empty %d/%d (target router off would still show neighbors)",
             s_rf_empty_scans, WIFI_RF_EMPTY_LIMIT);
    if (s_rf_empty_scans >= WIFI_RF_EMPTY_LIMIT) {
        wifi_rf_deep_sleep_recovery();
    }
    return false;
}

// Dedicated WiFi reconnect task with exponential backoff.
// Kept out of the system event loop so Zigbee signals are never blocked.
static void wifi_reconnect_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for first disconnect
        while (!s_wifi_ok) {
            int shift = s_wifi_retry < 6 ? s_wifi_retry : 6;
            int delay = WIFI_RETRY_BASE_MS * (1 << shift);
            if (delay > WIFI_RETRY_MAX_MS) delay = WIFI_RETRY_MAX_MS;
            ESP_LOGW(TAG, "WiFi lost, retry %d in %dms", s_wifi_retry, delay);

            // Check the whole 2.4GHz environment, not only the target SSID:
            // neighbors visible + target absent means wait for the router;
            // zero APs repeatedly means the local RF needs a deep power reset.
            if ((s_wifi_retry >= 2 && s_wifi_retry <= 4) ||
                (s_wifi_retry >= 8 && (s_wifi_retry % 4) == 0)) {
                wifi_rf_health_scan();
            }

            // Boot watchdog: if the radio never associated since power-on
            // (cold-boot RF glitch), perform a warm reboot like pressing RST.
            if (!s_wifi_ever_connected &&
                (int64_t)esp_timer_get_time() / 1000 > WIFI_BOOT_GRACE_MS) {
                int cnt = boot_restart_count_get();
                if (s_rf_environment_alive) {
                    ESP_LOGI(TAG,
                             "Boot watchdog hold: nearby APs are visible; target SSID absent or association pending, no reboot");
                } else if (cnt < WIFI_BOOT_MAX_RESTARTS) {
                    boot_restart_count_set(cnt + 1);
                    ESP_LOGW(TAG, "WiFi boot watchdog: warm reboot %d/%d",
                             cnt + 1, WIFI_BOOT_MAX_RESTARTS);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "WiFi still down after %d warm reboots; keep retrying", cnt);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(delay));
            if (!s_wifi_ok) {
                // Clear a possibly stuck association first, otherwise
                // esp_wifi_connect() can fail with "sta is connecting".
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "WiFi connect returned: %s", esp_err_to_name(err));
                }
                s_wifi_retry++;
                // Periodically recycle the WiFi state machine if it is wedged.
                if ((s_wifi_retry % 5) == 0) {
                    ESP_LOGW(TAG, "WiFi recovery: stop/start");
                    esp_wifi_stop();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_wifi_start();  // STA_START event triggers connect again
                }
            }
        }
    }
}

static void wifi_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi disconnected: reason=%d rssi=%d", disc->reason, disc->rssi);
        // Never block here: this callback runs in the system event loop task,
        // which also delivers Zigbee ZDO signals. Hand off to a reconnect task.
        s_wifi_ok = false;
        if (s_wifi_reconn_task) {
            xTaskNotifyGive(s_wifi_reconn_task);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "WiFi OK, IP: %s", s_ip);
        s_wifi_retry = 0;
        s_rf_empty_scans = 0;
        s_rf_environment_alive = true;
        s_wifi_ok = true;
        if (!s_wifi_ever_connected) {
            s_wifi_ever_connected = true;
            boot_restart_count_set(0);   // success: allow watchdog again next cold boot
        }
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    xTaskCreate(wifi_reconnect_task, "wifi-reconn", 5120, NULL, 4, &s_wifi_reconn_task);
    s_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
    }

    
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_cb, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_cb, NULL, NULL);
    
    // Keep Wi-Fi credentials/config in RAM instead of NVS. The home router may
    // change 2.4G channel (observed ch13 -> ch7), and stale cached BSSID/channel
    // data must not steer scans toward an old AP location.
    esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set RAM storage failed: %s", esp_err_to_name(storage_err));
    }

    // Scan every 2.4G channel before each association rather than using fast
    // scan. This tolerates router channel changes and transient AP restarts.
    wifi_config_t wcfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .bssid_set = false,
            .channel = 0,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .failure_retry_cnt = 3,
            .threshold = {
                .rssi = -127,
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);

    // Chinese home routers may select channel 12/13. The IDF default [01]
    // scans only 1-11; explicitly allow the CN 2.4GHz channel range.
    wifi_country_t country = {
        .cc = {'C', 'N'},
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    err = esp_wifi_set_country(&country);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set country failed: %s", esp_err_to_name(err));
    }

    // Give the active 802.15.4 coordinator as much shared-radio time as
    // possible. MAX_MODEM lets Wi-Fi sleep between DTIM/listen windows; MQTT
    // traffic here is low-rate, while ZED rejoin scans are latency sensitive.
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();

    // The STA must be started before changing default scan timing. A longer
    // active scan helps in crowded 2.4GHz environments and on channels 12/13.
    wifi_scan_default_params_t scan_params = {
        .scan_time.active.min = 0,
        .scan_time.active.max = 300,
        .home_chan_dwell_time = 30,
    };
    err = esp_wifi_set_scan_parameters(&scan_params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set scan parameters failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "WiFi starting: %s", WIFI_SSID);
}

// ==================== MQTT ====================

static cJSON *rf_command_payload_holder(cJSON *root)
{
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    return cJSON_IsObject(payload) ? payload : root;
}

static bool rf_command_get_i8(cJSON *holder, const char *key, int8_t *value)
{
    cJSON *item = cJSON_GetObjectItem(holder, key);
    double number = 0.0;

    if (!cJSON_IsNumber(item)) {
        return false;
    }
    number = item->valuedouble;
    if (number < -128.0 || number > 127.0 || number != (double)(int8_t)number) {
        return false;
    }
    *value = (int8_t)number;
    return true;
}

// Forward a cloud command delivered over MQTT to a Zigbee node as a
// standard ZCL On/Off cluster command. Topic convention:
//   {prefix}/{gateway}/nodes/{node}/cmd
// with {node} = "zb-XXXX" (short address). Payload: {"command":"on|off|toggle"}.
static void gateway_handle_node_cmd(const char *topic, const char *payload)
{
    const char *nodes = strstr(topic, "/nodes/");
    if (nodes == NULL) {
        return;
    }
    const char *node_start = nodes + strlen("/nodes/");
    const char *tail = strchr(node_start, '/');
    if (tail == NULL || strcmp(tail, "/cmd") != 0) {
        return;  // not a node command topic
    }

    char node[24];
    size_t node_len = (size_t)(tail - node_start);
    if (node_len == 0 || node_len >= sizeof(node)) {
        return;
    }
    memcpy(node, node_start, node_len);
    node[node_len] = '\0';

    if (strncmp(node, "zb-", 3) != 0) {
        return;
    }
    unsigned parsed = 0;
    if (sscanf(node + 3, "%x", &parsed) != 1 || parsed == 0 || parsed > 0xFFFF) {
        return;
    }
    uint16_t short_addr = (uint16_t)parsed;

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "CMD: non-JSON payload on %s", topic);
        return;
    }
    cJSON *command = cJSON_GetObjectItem(root, "command");
    cJSON *request_id_item = cJSON_GetObjectItem(root, "request_id");
    const char *request_id = cJSON_IsString(request_id_item)
                           ? request_id_item->valuestring : NULL;
    uint8_t cmd_id = 0xFF;
    if (cJSON_IsString(command)) {
        const char *command_name = command->valuestring;
        if (strcmp(command_name, "rf_query_power") == 0) {
            ESP_LOGI(TAG, "CMD rf: node=0x%04x query", (unsigned)short_addr);
            rf_power_remote_query(request_id);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(command_name, "rf_set_auto") == 0) {
            ESP_LOGI(TAG, "CMD rf: node=0x%04x automatic", (unsigned)short_addr);
            rf_power_remote_set_auto(request_id);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(command_name, "rf_set_power") == 0 ||
            strcmp(command_name, "rf_manual_power") == 0) {
            int8_t target_power_dbm = 0;
            cJSON *holder = rf_command_payload_holder(root);
            if (rf_command_get_i8(holder, "power_dbm", &target_power_dbm)) {
                ESP_LOGI(TAG, "CMD rf: node=0x%04x set %d dBm",
                         (unsigned)short_addr, (int)target_power_dbm);
                rf_power_remote_set_manual(target_power_dbm, request_id);
            } else {
                ESP_LOGW(TAG, "CMD rf: missing/invalid power_dbm");
            }
            cJSON_Delete(root);
            return;
        }
        if (strcmp(command->valuestring, "on") == 0) {
            cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_ON_ID;
        } else if (strcmp(command->valuestring, "off") == 0) {
            cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
        } else if (strcmp(command->valuestring, "toggle") == 0) {
            cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID;
        }
    }
    if (cmd_id == 0xFF) {
        ESP_LOGW(TAG, "CMD: unsupported command for node 0x%04x", short_addr);
        cJSON_Delete(root);
        return;
    }

    esp_zb_zcl_on_off_cmd_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = EP_GW,  // node endpoint 10
            .src_endpoint = EP_GW,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = cmd_id,
    };
    uint8_t zcl_seq = esp_zb_zcl_on_off_cmd_req(&req);
    ESP_LOGI(TAG, "CMD fwd: node=0x%04x cmd=%s zcl_seq=%u",
             (unsigned)short_addr, command->valuestring, (unsigned)zcl_seq);
    cJSON_Delete(root);
}

static void mqtt_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_ok = true;
        s_zb_status_resync = true;
        char sub[64];
        snprintf(sub, sizeof(sub), "%s/nodes/+/cmd", MQTT_TOPIC_PREFIX);
        esp_mqtt_client_subscribe(s_mqtt, sub, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_ok = false;
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT rx: %.*s = %.*s", e->topic_len, e->topic, e->data_len, e->data);
        {
            char topic[128];
            char payload[256];
            int tlen = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
            int dlen = e->data_len < (int)sizeof(payload) - 1 ? e->data_len : (int)sizeof(payload) - 1;
            memcpy(topic, e->topic, tlen);
            topic[tlen] = '\0';
            memcpy(payload, e->data, dlen);
            payload[dlen] = '\0';
            gateway_handle_node_cmd(topic, payload);
        }
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    static const char gw_lwt_payload[] =
        "{\"status\":\"offline\",\"event\":\"lwt\"}";
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will = {
            .topic = MQTT_TOPIC_PREFIX "/" MQTT_GW_STATUS_SUFFIX,
            .msg = gw_lwt_payload,
            .msg_len = sizeof(gw_lwt_payload) - 1,
            .qos = 1,
            .retain = true,
        },
    };
    if (s_mqtt_outbox == NULL) {
        s_mqtt_outbox = xQueueCreateStatic(
            MQTT_OUTBOX_LEN, sizeof(mqtt_outbox_msg_t),
            (uint8_t *)s_mqtt_outbox_storage, &s_mqtt_outbox_struct);
    }

    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_cb, NULL);
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "MQTT starting: %s", MQTT_BROKER_URI);
}

// Outage buffer: while Wi-Fi/MQTT is down, telemetry used to be dropped
// silently. A bounded FIFO keeps the newest MQTT_OUTBOX_LEN non-retained
// messages (~5 minutes at one frame per 10 s); when full, oldest frames are
// dropped first. Retained status messages stay best-effort because a
// status_resync is published automatically after every reconnect.


#define MQTT_MESSAGE_ID_MAX              48
#define MQTT_ENRICHED_PAYLOAD_MAX       192

static uint32_t s_mqtt_boot_epoch;
static uint32_t s_mqtt_message_seq;
static bool s_mqtt_message_context_ready;

static esp_err_t mqtt_message_context_init(void)
{
    if (s_mqtt_message_context_ready) {
        return ESP_OK;
    }

    nvs_handle_t nvh;
    esp_err_t err = nvs_open("gwmqtt", NVS_READWRITE, &nvh);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t boot_epoch = 0;
    // NVS 首次没有该键时使用默认值 0；随后递增，保证重启后编号不重复。
    (void)nvs_get_u32(nvh, "boot_epoch", &boot_epoch);
    s_mqtt_boot_epoch = boot_epoch + 1U;

    err = nvs_set_u32(nvh, "boot_epoch", s_mqtt_boot_epoch);
    if (err == ESP_OK) {
        err = nvs_commit(nvh);
    }
    nvs_close(nvh);

    if (err != ESP_OK) {
        return err;
    }

    s_mqtt_message_seq = 0;
    s_mqtt_message_context_ready = true;
    return ESP_OK;
}

static void mqtt_make_message_id(char *out, size_t size)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

    if (mqtt_message_context_init() == ESP_OK) {
        uint32_t seq = ++s_mqtt_message_seq;
        snprintf(out, size,
                 "%02x%02x%02x%02x%02x%02x-b%08lu-t%08lu-s%05lu",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned long)s_mqtt_boot_epoch,
                 (unsigned long)now_ms,
                 (unsigned long)seq);
    } else {
        // NVS 不可用时仍用硬件随机数给出近似全局唯一编号，避免阻断遥测；
        // 启动后 NVS 恢复时，下一帧会重新走持久 boot epoch。
        snprintf(out, size,
                 "%02x%02x%02x%02x%02x%02x-r%08lx-t%08lu",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned long)esp_random(),
                 (unsigned long)now_ms);
    }
}

static void mqtt_payload_with_message_id(
    const char *data, char *out, size_t out_size)
{
    const char *p = data;
    while (isspace((unsigned char)*p)) {
        p++;
    }

    if (*p != '{') {
        strlcpy(out, data, out_size);
        return;
    }

    char message_id[MQTT_MESSAGE_ID_MAX];
    mqtt_make_message_id(message_id, sizeof(message_id));

    char prefix[80];
    int prefix_len = snprintf(
        prefix, sizeof(prefix),
        "{\"message_id\":\"%s\",", message_id
    );
    const char *rest = p + 1;
    size_t rest_len = strlen(rest);

    if (prefix_len <= 0 ||
        (size_t)prefix_len + rest_len + 1U > out_size) {
        strlcpy(out, data, out_size);
        return;
    }

    memcpy(out, prefix, (size_t)prefix_len);
    memcpy(out + prefix_len, rest, rest_len);
    out[prefix_len + rest_len] = '\0';
}

static bool mqtt_try_publish(const char *topic_suffix, const char *data,
                             bool retain)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s", MQTT_TOPIC_PREFIX, topic_suffix);
    // Returns message id (>0) when accepted, -1 when the client cannot send.
    int msg_id = esp_mqtt_client_publish(s_mqtt, topic, data, 0, 1,
                                         retain ? 1 : 0);
    return msg_id >= 0;
}

static void mqtt_outbox_store(const char *topic_suffix, const char *data)
{
    mqtt_outbox_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strlcpy(msg.topic_suffix, topic_suffix, sizeof(msg.topic_suffix));
    strlcpy(msg.payload, data, sizeof(msg.payload));

    if (xQueueSend(s_mqtt_outbox, &msg, 0) != pdTRUE) {
        mqtt_outbox_msg_t dropped;
        // Bounded buffer: discard the oldest telemetry first.
        xQueueReceive(s_mqtt_outbox, &dropped, 0);
        xQueueSend(s_mqtt_outbox, &msg, 0);
        ESP_LOGW(TAG, "MQTT outbox full; oldest telemetry dropped: %s",
                 dropped.topic_suffix);
    }
}

void mqtt_pub_retained(const char *topic_suffix, const char *data, bool retain)
{
    // Retained status is republished by zb_publish_known_statuses() after a
    // reconnect, so it does not need to occupy the telemetry outbox.
    if (retain) {
        if (s_mqtt_ok) {
            mqtt_try_publish(topic_suffix, data, true);
        }
        return;
    }

    if (s_mqtt_ok && uxQueueMessagesWaiting(s_mqtt_outbox) == 0) {
        if (mqtt_try_publish(topic_suffix, data, false)) {
            return;
        }
        ESP_LOGW(TAG, "MQTT publish rejected; buffering telemetry: %s",
                 topic_suffix);
    }
    mqtt_outbox_store(topic_suffix, data);
}

void mqtt_pub(const char *topic_suffix, const char *data)
{
    char enriched[MQTT_ENRICHED_PAYLOAD_MAX];
    mqtt_payload_with_message_id(data, enriched, sizeof(enriched));
    mqtt_pub_retained(topic_suffix, enriched, false);
}

// Drain buffered telemetry oldest-first after reconnect. Called from the
// Zigbee forward task loop. Stops early if the MQTT client rejects a frame;
// the same message is retried on the next cycle.
static void mqtt_outbox_flush(void)
{
    if (!s_mqtt_ok) {
        return;
    }

    int flushed = 0;
    while (flushed < 10) {
        mqtt_outbox_msg_t msg;
        if (xQueuePeek(s_mqtt_outbox, &msg, 0) != pdTRUE) {
            break;
        }
        if (!mqtt_try_publish(msg.topic_suffix, msg.payload, false)) {
            break;
        }
        xQueueReceive(s_mqtt_outbox, &msg, 0);
        flushed++;
    }

    if (flushed > 0) {
        ESP_LOGI(TAG, "MQTT outbox flushed: %d buffered message(s)", flushed);
    }
}

// ==================== Tasks ====================

static void oled_task(void *arg)
{
    // Two-colour SSD1306: rows 0-15 are physically YELLOW (title),
    // rows 16-63 are BLUE (content). Compact font is 5x7 with 6px pitch.
    while (1) {
        int64_t us = esp_timer_get_time() / 1000000;
        int h = (int)(us / 3600), m = (int)((us % 3600) / 60), sec = (int)(us % 60);
        char line[24];

        if (s_oled_available) {
            oled_clear();

            // Yellow band: 11 characters * 6px = 66px, x=(128-66)/2=31.
            oled_text(31, 4, "IOT-HOME GW");

            // Blue band: compact status lines.
            snprintf(line, sizeof(line), "IP:%s", s_ip);
            oled_text(0, 18, line);

            snprintf(line, sizeof(line), "WIFI:%s MQTT:%s",
                     s_wifi_ok ? "OK" : "NO",
                     s_mqtt_ok ? "OK" : "NO");
            oled_text(0, 30, line);

            snprintf(line, sizeof(line), "UP:%02d:%02d:%02d", h, m, sec);
            oled_text(0, 42, line);

            snprintf(line, sizeof(line), "ZB NODES:%d", s_zigbee_devices);
            oled_text(0, 54, line);

            oled_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(OLED_UPDATE_MS));
    }
}

static void status_task(void *arg)
{
    while (1) {
        if (s_mqtt_ok) {
            int64_t us = esp_timer_get_time() / 1000000;
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"status\":\"online\",\"event\":\"heartbeat\",\"ip\":\"%s\",\"uptime\":%lld}",
                     s_ip, us);
            mqtt_pub_retained(MQTT_GW_STATUS_SUFFIX, msg, true);
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}

// ==================== Zigbee -> MQTT forwarding ====================

#define ZB_FORWARD_INTERVAL_MS  2000

static void zb_persist_child_seen(void)
{
    if (s_child_seen) {
        return;
    }
    s_child_seen = true;

    nvs_handle_t nvh;
    if (nvs_open("gwzb", NVS_READWRITE, &nvh) == ESP_OK) {
        nvs_set_u8(nvh, "child", 1);
        nvs_commit(nvh);
        nvs_close(nvh);
    }
}

static void zb_publish_gateway_status(const char *event)
{
    if (!s_mqtt_ok) {
        return;
    }

    int64_t us = esp_timer_get_time() / 1000000;
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"status\":\"online\",\"event\":\"%s\",\"ip\":\"%s\",\"uptime\":%lld}",
             event, s_ip, us);
    mqtt_pub_retained(MQTT_GW_STATUS_SUFFIX, msg, true);
}

static void zb_publish_node_status(const zb_node_t *node, const char *event, bool online)
{
    if (!s_mqtt_ok || node == NULL || node->addr == 0) {
        return;
    }

    char topic_suffix[40];
    char data[144];
    snprintf(topic_suffix, sizeof(topic_suffix), "nodes/zb-%04x/status", node->addr);
    snprintf(data, sizeof(data),
             "{\"status\":\"%s\",\"event\":\"%s\",\"ieee\":\"%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\"}",
             online ? "online" : "offline", event,
             node->ieee[7], node->ieee[6], node->ieee[5], node->ieee[4],
             node->ieee[3], node->ieee[2], node->ieee[1], node->ieee[0]);
    mqtt_pub_retained(topic_suffix, data, true);
    ESP_LOGI(TAG, "MQTT: %s status retained: %s", topic_suffix, event);
}

static void zb_publish_known_statuses(void)
{
    zb_node_t snap[MAX_ZB_NODES];
    int n_active = 0;

    portENTER_CRITICAL(&s_zb_lock);
    for (int i = 0; i < MAX_ZB_NODES; i++) {
        if (s_zb_nodes[i].addr != 0 && s_zb_nodes[i].active) {
            snap[n_active++] = s_zb_nodes[i];
        }
    }
    portEXIT_CRITICAL(&s_zb_lock);

    zb_publish_gateway_status("status_resync");
    for (int i = 0; i < n_active; i++) {
        zb_publish_node_status(&snap[i], "status_resync", true);
    }
}

static void zb_handle_join_event(const zb_app_event_t *event)
{
    zb_node_t *node;
    zb_node_t status_snap = {0};
    bool is_new_child = false;

    portENTER_CRITICAL(&s_zb_lock);
    node = zb_node_get_or_create(event->addr);
    if (node != NULL) {
        if (!node->active) {
            node->active = true;
            is_new_child = true;
            s_zigbee_devices++;
        }
        memcpy(node->ieee, event->ieee, sizeof(node->ieee));
        status_snap = *node;
    }
    portEXIT_CRITICAL(&s_zb_lock);

    if (node == NULL) {
        ESP_LOGW(TAG, "Zigbee: join event ignored, node table full addr=0x%04x", event->addr);
        return;
    }

    if (is_new_child) {
        ESP_LOGI(TAG, "Zigbee: Device joined! addr=0x%04x", event->addr);
        zb_persist_child_seen();
    } else {
        ESP_LOGI(TAG, "Zigbee: Device announced again! addr=0x%04x", event->addr);
    }

    zb_publish_node_status(&status_snap, is_new_child ? "device_joined" : "device_announce", true);
    rf_power_reset_for_announce(event->addr);
    rf_power_publish_querying(event->addr, NULL);
}

static void zb_handle_leave_event(const zb_app_event_t *event)
{
    zb_node_t snap;
    bool was_active = false;

    portENTER_CRITICAL(&s_zb_lock);
    for (int i = 0; i < MAX_ZB_NODES; i++) {
        if (s_zb_nodes[i].addr != 0 &&
            memcmp(s_zb_nodes[i].ieee, event->ieee, sizeof(event->ieee)) == 0) {
            if (s_zb_nodes[i].active) {
                snap = s_zb_nodes[i];
                s_zb_nodes[i].active = false;
                s_zb_nodes[i].dirty = 0;
                s_zigbee_devices--;
                was_active = true;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_zb_lock);

    if (was_active) {
        ESP_LOGI(TAG, "Zigbee: Device left! addr=0x%04x", snap.addr);
        rf_power_forget_node(snap.addr);
        zb_publish_node_status(&snap, "device_left", false);
    } else {
        ESP_LOGI(TAG, "Zigbee: Leave for unknown/inactive IEEE "
                      "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 event->ieee[7], event->ieee[6], event->ieee[5], event->ieee[4],
                 event->ieee[3], event->ieee[2], event->ieee[1], event->ieee[0]);
    }
}

static void zb_publish_node_switch_state(const zb_node_t *node)
{
    char topic_suffix[40];
    char payload[64];

    if (!s_mqtt_ok || node == NULL || !node->onoff_valid) {
        return;
    }

    snprintf(topic_suffix, sizeof(topic_suffix), "nodes/zb-%04x/telemetry", node->addr);
    snprintf(payload, sizeof(payload), "{\"data\":{\"on_off\":%u}}", node->onoff_value);
    mqtt_pub(topic_suffix, payload);
    ESP_LOGI(TAG, "MQTT -> %s: %s", topic_suffix, payload);
}

static void zb_handle_report_event(const zb_app_event_t *event)
{
    bool became_active = false;
    bool first_dirty = false;
    bool need_status = false;
    bool switch_state_changed = false;
    zb_node_t status_snap = {0};
    zb_node_t switch_snap = {0};
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

    portENTER_CRITICAL(&s_zb_lock);
    zb_node_t *node = zb_node_get_or_create(event->addr);
    if (node) {
        // A successful report proves the child is present even if an announce was dropped.
        if (!node->active) {
            node->active = true;
            became_active = true;
            s_zigbee_devices++;
        }
        first_dirty = (node->dirty == 0);

        switch (event->cluster) {
        case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:
            node->temp_raw = (int16_t)event->raw;
            node->valid |= ZB_MASK_TEMP;
            node->dirty |= ZB_MASK_TEMP;
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
            node->hum_raw = event->raw;
            node->valid |= ZB_MASK_HUM;
            node->dirty |= ZB_MASK_HUM;
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT:
            node->lux_raw = event->raw;
            node->valid |= ZB_MASK_LUX;
            node->dirty |= ZB_MASK_LUX;
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
            node->onoff_value = event->value;
            node->onoff_valid = true;
            switch_snap = *node;
            switch_state_changed = true;
            break;
        default:
            break;
        }

        if (became_active) {
            status_snap = *node;
            need_status = true;
        }
        if (first_dirty) {
            node->first_dirty_ms = now_ms;
        }
        node->last_report_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_zb_lock);

    if (node) {
        if (became_active) {
            ESP_LOGI(TAG, "Zigbee: Device active by report addr=0x%04x", event->addr);
            zb_persist_child_seen();
            rf_power_reset_for_announce(event->addr);
            rf_power_publish_querying(event->addr, NULL);
            if (need_status) {
                zb_publish_node_status(&status_snap, "device_active", true);
            }
        }
        if (switch_state_changed) {
            zb_publish_node_switch_state(&switch_snap);
        }
        s_zb_report_events++;
        unsigned event_value = event->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF
                             ? event->value : event->raw;
        ESP_LOGI(TAG, "Zigbee: cached report node=0x%04x cluster=0x%04x value=%u total=%lu",
                 event->addr, event->cluster, event_value,
                 (unsigned long)s_zb_report_events);
    }
}

static uint32_t s_zb_last_telemetry_ms[MAX_ZB_NODES];
static char s_zb_last_telemetry_payload[MAX_ZB_NODES][128];

static void zb_handle_signal_event(const zb_app_event_t *event)
{
    esp_err_t status = (esp_err_t)event->status;
    esp_zb_app_signal_type_t signal =
        (esp_zb_app_signal_type_t)event->signal;

    switch (signal) {
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (status == ESP_OK) {
            if (event->value == 0) {
                ESP_LOGI(TAG, "Zigbee: permit joining closed; secure rejoin may still use existing credentials");
            } else if (event->value == 0xff) {
                ESP_LOGI(TAG, "Zigbee: permit joining enabled (duration=0xff)");
            } else {
                ESP_LOGI(TAG, "Zigbee: permit joining open for %u seconds",
                         (unsigned)event->value);
            }
        } else {
            ESP_LOGW(TAG, "Zigbee: permit join status failed: %s",
                     esp_err_to_name(status));
        }
        break;
    case ESP_ZB_SE_SIGNAL_REJOIN:
        ESP_LOGI(TAG, "Zigbee: security rejoin event signal=%d status=%s",
                 (int)signal, esp_err_to_name(status));
        break;
    case ESP_ZB_SE_SIGNAL_CHILD_REJOIN:
        ESP_LOGI(TAG, "Zigbee: security child rejoin signal=%d status=%s",
                 (int)signal, esp_err_to_name(status));
        break;
    case ESP_ZB_BDB_SIGNAL_TC_REJOIN_DONE:
        ESP_LOGI(TAG, "Zigbee: Trust Center rejoin done status=%s",
                 esp_err_to_name(status));
        break;
    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        ESP_LOGW(TAG, "Zigbee: link/service unavailable signal=%d status=%s",
                 (int)signal, esp_err_to_name(status));
        break;
    case ESP_ZB_NLME_STATUS_INDICATION:
        ESP_LOGW(TAG,
                 "Zigbee: NLME status indication nwk_status=0x%02x "
                 "network_addr=0x%04x unknown_cmd=%u signal_status=%s",
                 (unsigned)event->value, event->addr,
                 (unsigned)event->raw, esp_err_to_name(status));
        break;
    default:
        ESP_LOGI(TAG, "Zigbee: diagnostic signal=%d status=%s",
                 (int)signal, esp_err_to_name(status));
        break;
    }
}

static void zb_forward_pending(void)
{
    if (!s_mqtt_ok) {
        return;
    }

    // Wait for a complete temperature/humidity/light sample to avoid duplicate
    // MQTT messages; sleepy-end-device reports can arrive more than 10s apart.
    // A longer partial timeout still publishes incomplete/legacy sensors.
    zb_node_t snap[MAX_ZB_NODES];
    int n_dirty = 0;
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    portENTER_CRITICAL(&s_zb_lock);
    for (int i = 0; i < MAX_ZB_NODES; i++) {
        zb_node_t *node = &s_zb_nodes[i];
        if (node->addr == 0 || !node->active || node->dirty == 0) {
            continue;
        }
        bool complete = (node->dirty & ZB_MASK_ALL) == ZB_MASK_ALL;
        uint32_t age_ms = now_ms - node->first_dirty_ms;
        if (complete || age_ms >= ZB_REPORT_PARTIAL_DELAY_MS) {
            snap[n_dirty++] = *node;
            node->dirty = 0;
        }
    }
    portEXIT_CRITICAL(&s_zb_lock);

    for (int i = 0; i < n_dirty; i++) {
        zb_node_t *n = &snap[i];
        char topic_suffix[40];
        snprintf(topic_suffix, sizeof(topic_suffix), "nodes/zb-%04x/telemetry", n->addr);

        char payload[160];
        int pos = snprintf(payload, sizeof(payload), "{\"data\":{");
        if (n->valid & ZB_MASK_TEMP) {
            pos += snprintf(payload + pos, sizeof(payload) - pos,
                            "\"temperature\":%.1f,", n->temp_raw / 100.0f);
        }
        if (n->valid & ZB_MASK_HUM) {
            pos += snprintf(payload + pos, sizeof(payload) - pos,
                            "\"humidity\":%.1f,", n->hum_raw / 100.0f);
        }
        if (n->valid & ZB_MASK_LUX) {
            // raw = 10000 * log10(lux + 1)  ->  lux = 10^(raw/10000) - 1
            float lux = powf(10.0f, (float)n->lux_raw / 10000.0f) - 1.0f;
            pos += snprintf(payload + pos, sizeof(payload) - pos,
                            "\"lux\":%.1f,", lux);
        }
        if (n->onoff_valid) {
            pos += snprintf(payload + pos, sizeof(payload) - pos,
                            "\"on_off\":%u,", (unsigned)n->onoff_value);
        }
        if (payload[pos - 1] == ',') pos--;
        snprintf(payload + pos, sizeof(payload) - pos, "}}");

        uint32_t telemetry_age_ms = now_ms - s_zb_last_telemetry_ms[i];
        bool duplicate_telemetry =
            s_zb_last_telemetry_payload[i][0] != '\0' &&
            telemetry_age_ms < ZB_TELEMETRY_DEDUP_MS &&
            strcmp(payload, s_zb_last_telemetry_payload[i]) == 0;

        if (duplicate_telemetry) {
            ESP_LOGI(TAG, "MQTT duplicate suppressed (%s, age=%lu ms): %s",
                     topic_suffix, (unsigned long)telemetry_age_ms, payload);
        } else {
            mqtt_pub(topic_suffix, payload);
            ESP_LOGI(TAG, "MQTT -> %s: %s", topic_suffix, payload);
            strlcpy(s_zb_last_telemetry_payload[i], payload,
                    sizeof(s_zb_last_telemetry_payload[i]));
            s_zb_last_telemetry_ms[i] = now_ms;
        }
    }
}

static bool zb_have_full_active_sample(void)
{
    bool ready = false;

    portENTER_CRITICAL(&s_zb_lock);
    for (int i = 0; i < MAX_ZB_NODES; i++) {
        if (s_zb_nodes[i].addr != 0 &&
            s_zb_nodes[i].active &&
            (s_zb_nodes[i].valid & ZB_MASK_ALL) == ZB_MASK_ALL) {
            ready = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_zb_lock);
    return ready;
}

static void zb_handle_app_event(const zb_app_event_t *event)
{
    if (event->type == ZB_APP_EVENT_JOIN) {
        zb_handle_join_event(event);
    } else if (event->type == ZB_APP_EVENT_REPORT) {
        zb_handle_report_event(event);
    } else if (event->type == ZB_APP_EVENT_LEAVE) {
        zb_handle_leave_event(event);
    } else {
        ESP_LOGW(TAG, "Zigbee: unexpected business event type=%d",
                 (int)event->type);
    }
}

static void zb_forward_task(void *arg)
{
    TickType_t last_app_drop_log_tick = xTaskGetTickCount();
    TickType_t last_signal_drop_log_tick = xTaskGetTickCount();
    uint32_t reported_app_drops = 0;
    uint32_t reported_signal_drops = 0;

    while (1) {
        zb_app_event_t event;
        QueueSetMemberHandle_t active_queue =
            xQueueSelectFromSet(s_zb_queue_set,
                                pdMS_TO_TICKS(ZB_FORWARD_INTERVAL_MS));

        if (active_queue == s_zb_app_q) {
            if (xQueueReceive(s_zb_app_q, &event, 0) == pdTRUE) {
                zb_handle_app_event(&event);
            }
        } else if (active_queue == s_zb_signal_q) {
            if (xQueueReceive(s_zb_signal_q, &event, 0) == pdTRUE) {
                zb_handle_signal_event(&event);
            }
        }

        if (s_zb_status_resync && s_mqtt_ok) {
            s_zb_status_resync = false;
            zb_publish_known_statuses();
        }
        zb_forward_pending();
        rf_power_controller_periodic();
        mqtt_outbox_flush();

        uint32_t app_drops = s_zb_app_drops_task + s_zb_app_drops_isr;
        if (app_drops != reported_app_drops &&
            (TickType_t)(xTaskGetTickCount() - last_app_drop_log_tick)
                >= pdMS_TO_TICKS(60000)) {
            ESP_LOGW(TAG,
                     "Zigbee: business event queue drops=%lu (task=%lu isr=%lu)",
                     (unsigned long)app_drops,
                     (unsigned long)s_zb_app_drops_task,
                     (unsigned long)s_zb_app_drops_isr);
            reported_app_drops = app_drops;
            last_app_drop_log_tick = xTaskGetTickCount();
        }

        uint32_t signal_drops = s_zb_signal_drops_task + s_zb_signal_drops_isr;
        if (signal_drops != reported_signal_drops &&
            (TickType_t)(xTaskGetTickCount() - last_signal_drop_log_tick)
                >= pdMS_TO_TICKS(60000)) {
            ESP_LOGW(TAG,
                     "Zigbee: diagnostic event queue drops=%lu (task=%lu isr=%lu)",
                     (unsigned long)signal_drops,
                     (unsigned long)s_zb_signal_drops_task,
                     (unsigned long)s_zb_signal_drops_isr);
            reported_signal_drops = signal_drops;
            last_signal_drop_log_tick = xTaskGetTickCount();
        }
    }
}

// ==================== Main ====================

void app_main(void)
{
    ESP_LOGI(TAG, "=============================");
    ESP_LOGI(TAG, "IoT-Home Gateway v3.0 (Zigbee)");
    ESP_LOGI(TAG, "=============================");
    
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    rf_power_policy_init();

    {
        nvs_handle_t nvh;
        uint8_t formed = 0;
        uint8_t child = 0;
        if (nvs_open("gwzb", NVS_READONLY, &nvh) == ESP_OK) {
            nvs_get_u8(nvh, "formed", &formed);
            nvs_get_u8(nvh, "child", &child);
            nvs_close(nvh);
        }
        s_network_formed = (formed != 0);
        s_child_seen = (child != 0);

    }

#if !ZB_ONLY_RF_DIAG && CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
    esp_ieee802154_set_coex_config((esp_ieee802154_coex_config_t){
        .idle = IEEE802154_IDLE,
        .txrx = IEEE802154_LOW,
        .txrx_at = IEEE802154_MIDDLE,
    });
    ESP_LOGI(TAG, "Wi-Fi/IEEE 802.15.4 coexistence enabled (15.4 priority LOW/MIDDLE; Wi-Fi MAX_MODEM)");
#else
    ESP_LOGW(TAG, "ZB-only RF diagnostic: Wi-Fi/coexistence disabled");
#endif
    
    // I2C / OLED bus
    ret = oled_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C init failed (SDA=%d SCL=%d): %s",
                 I2C_SDA_PIN, I2C_SCL_PIN, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "I2C init OK (SDA=%d SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);
    }
    
    // OLED: local verified SSD1306 128x64 text driver
    if (oled_check_device() && oled_init()) {
        s_oled_available = true;
        oled_clear();
        oled_text(31, 4, "IOT-HOME GW");
        oled_text(31, 30, "STARTING...");
        if (oled_flush()) {
            ESP_LOGI(TAG, "OLED init OK (local SSD1306 text driver)");
        } else {
            ESP_LOGW(TAG, "OLED initialized but first flush failed");
        }
    } else {
        ESP_LOGW(TAG, "OLED not available, running without display");
    }

    
#if !ZB_ONLY_RF_DIAG && !ZB_JOIN_BEFORE_WIFI
    // WiFi
    wifi_start();
    xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    // MQTT
    mqtt_start();
#elif ZB_ONLY_RF_DIAG
    ESP_LOGW(TAG, "ZB-only RF diagnostic: WiFi and MQTT not started");
#endif

    // Business and diagnostic events use independent queues. A signal storm
    // can never consume business-event capacity or block Zigbee callbacks.
    s_zb_queue_set = xQueueCreateSet(ZB_APP_QUEUE_LEN + ZB_SIGNAL_QUEUE_LEN);
    if (s_zb_queue_set == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_zb_app_q = xQueueCreate(ZB_APP_QUEUE_LEN, sizeof(zb_app_event_t));
    s_zb_signal_q = xQueueCreate(ZB_SIGNAL_QUEUE_LEN, sizeof(zb_app_event_t));
    if (s_zb_app_q == NULL || s_zb_signal_q == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    if (xQueueAddToSet(s_zb_app_q, s_zb_queue_set) != pdTRUE ||
        xQueueAddToSet(s_zb_signal_q, s_zb_queue_set) != pdTRUE) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    // Tasks
    // Zigbee
    xTaskCreate(zigbee_task, "zigbee", 16384, NULL, 5, NULL);

    xTaskCreate(oled_task, "oled", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);
    xTaskCreate(zb_forward_task, "zb-fwd", 4096, NULL, 5, NULL);

#if !ZB_ONLY_RF_DIAG && ZB_JOIN_BEFORE_WIFI
    // Give commissioning a contention-free 802.15.4 window. Once a node is
    // present, bring up Wi-Fi/MQTT and rely on balanced runtime coexistence.
    const int join_first_timeout_ms =
        s_child_seen ? ZB_REJOIN_FIRST_TIMEOUT_MS : ZB_JOIN_FIRST_TIMEOUT_MS;
    ESP_LOGI(TAG, "Zigbee %s RF window before Wi-Fi: %d ms",
             s_child_seen ? "rejoin" : "commissioning", join_first_timeout_ms);

    int join_wait_ms = 0;
    while (join_wait_ms < join_first_timeout_ms && s_zigbee_devices == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        join_wait_ms += 500;
    }
    bool join_seen = (s_zigbee_devices > 0);
    bool first_sample_ready = false;
    if (join_seen) {
        ESP_LOGI(TAG, "Zigbee join announced; keep Wi-Fi off until a full sensor sample");
        while (join_wait_ms < join_first_timeout_ms) {
            if (zb_have_full_active_sample()) {
                first_sample_ready = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            join_wait_ms += 500;
        }
    }

    if (!join_seen) {
        ESP_LOGW(TAG, "No Zigbee join in %d ms; start Wi-Fi anyway", join_first_timeout_ms);
    } else if (first_sample_ready) {
        ESP_LOGI(TAG, "Full Zigbee sample received at %d ms; start IP stack after cache window",
                 join_wait_ms);
        // Allow two 10-second node samples to receive APS confirmation before
        // Wi-Fi association adds RF coexistence pressure during startup.
        vTaskDelay(pdMS_TO_TICKS(12000));
    } else {
        ESP_LOGW(TAG, "Join seen but no full sample in %d ms; start Wi-Fi anyway",
                 join_first_timeout_ms);
    }

    wifi_start();
    xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    mqtt_start();
#endif
    
    ESP_LOGI(TAG, "Gateway ready!");
    
    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        if (s_mqtt_ok) {
            char msg[64];
            snprintf(msg, sizeof(msg), "{\"gw\":\"online\"}");
            mqtt_pub("nodes/gw-001/telemetry", msg);
        }
    }
}
