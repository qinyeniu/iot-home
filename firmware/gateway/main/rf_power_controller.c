/*
 * RF power controller - gateway-side automatic TX power control.
 *
 * Extracted from main.c so the gateway application file stays focused on
 * orchestration. This component owns the RF power policy state machine,
 * Zigbee APS report handling, QUERY/SET/AUTO commands, TX backoff logic
 * and the MAC no-buffer health restart.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_attribute.h"
#include "nwk/esp_zigbee_nwk.h"
#include "aps/esp_zigbee_aps.h"
#include "esp_ieee802154.h"
#include "iot_rf_packet.h"
#include "iot_rf_power_policy.h"
#include "iot_rf_transaction.h"
#include "iot_zb_node_model.h"
#include "cJSON.h"
#include "rf_power_controller.h"
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
#include "rf_command_queue.h"
#include "rf_dispatch_plan.h"
#include "rf_background_command_mapper.h"
#include "rf_background_dispatch_plan.h"
#include "rf_background_status_plan.h"
#include "iot_provisioning_recovery_quiesce.h"
#include "iot_usb_serial_jtag.h"
#endif

/* Configuration copied from main.c */
#define RF_POWER_UNKNOWN_DBM       (-128)
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
#define RF_POLICY_GENERATION_ANY  UINT32_MAX
#define RF_POWER_FRESH_MS         120000
#define RF_POWER_SAMPLES_NEEDED   6
#define RF_LQI_LOWER              240
#define RF_LQI_LOWER_WINDOWS      3
#define RF_POWER_SETTLE_MS        90000
#define RF_POWER_RECONCILE_MS     300000
#define RF_TX_BACKOFF_FIRST_MS   300000
#define RF_TX_BACKOFF_SECOND_MS  600000
#define RF_TX_BACKOFF_MAX_MS     1800000
#define RF_HEALTH_NO_RX_RESTART_MS 180000
#define RF_HEALTH_MAC_NOBUFFER_EVENTS 100
#define RF_HEALTH_MAC_NOBUFFER_SPAN_MS 60000
#define RF_HEALTH_MAC_NOBUFFER_FRESH_MS 5000
#define RF_POLICY_NVS_NAMESPACE   "rfpol"

static const char *TAG = "iot-gw";

/* Registered callbacks (set by main.c) */
static rf_power_controller_mqtt_pub_t s_mqtt_publish = NULL;
static rf_power_controller_active_child_cb_t s_active_child_check = NULL;

void rf_power_controller_set_mqtt_publisher(rf_power_controller_mqtt_pub_t pub) { s_mqtt_publish = pub; }
void rf_power_controller_set_active_child_check(rf_power_controller_active_child_cb_t cb) { s_active_child_check = cb; }
/* Forward declaration (defined in the health-check section at the bottom). */
static void rf_health_log_init(void);

// ==================== Gateway-side automatic TX power control ====================

static const int8_t s_rf_power_table[] = {-10, 0, 8, 14, 18, 20};
#define RF_POWER_LEVEL_COUNT (sizeof(s_rf_power_table) / sizeof(s_rf_power_table[0]))
static const iot_rf_power_policy_config_t s_rf_power_policy_config = {
    .levels_dbm = s_rf_power_table,
    .level_count = (uint8_t)RF_POWER_LEVEL_COUNT,
    .min_power_dbm = RF_POWER_AUTO_MIN_DBM,
    .max_power_dbm = RF_POWER_AUTO_MAX_DBM,
    .lqi_good_threshold = RF_LQI_LOWER,
    .good_windows_needed = RF_LQI_LOWER_WINDOWS,
    .samples_needed = RF_POWER_SAMPLES_NEEDED,
};
static const iot_rf_transaction_config_t s_rf_transaction_config = {
    .link_fresh_ms = RF_POWER_FRESH_MS,
    .report_timeout_ms = RF_POWER_SETTLE_MS,
    .reconcile_ms = RF_POWER_RECONCILE_MS,
};



static uint16_t s_rf_node_addr;
static int s_rf_lqi_ema = -1;
static uint8_t s_rf_lqi_samples;
static uint32_t s_rf_last_rx_ms;
static int8_t s_rf_node_power_dbm = RF_POWER_UNKNOWN_DBM;
static uint8_t s_rf_node_mode = IOT_RF_MODE_UNKNOWN;
static uint8_t s_rf_packet_sequence;
static uint32_t s_rf_policy_generation;
static uint32_t s_rf_last_eval_ms;
static uint8_t s_rf_lqi_low_windows;
static uint8_t s_rf_lqi_high_windows;
static bool s_rf_report_waiting;
static uint8_t s_rf_expected_report_seq;
static int8_t s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
static uint8_t s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
static bool s_rf_last_report_seq_valid;
static uint8_t s_rf_last_report_seq;
static uint32_t s_rf_wait_started_ms;
static uint32_t s_rf_power_confirmed_ms;
static uint8_t s_rf_tx_fail_count;
static uint32_t s_rf_tx_backoff_until_ms;
static volatile uint32_t s_rf_mac_nobuffer_events;
static volatile uint32_t s_rf_first_mac_nobuffer_ms;
static volatile uint32_t s_rf_last_mac_nobuffer_ms;
static vprintf_like_t s_original_log_vprintf;
static uint8_t s_rf_mode;
static int8_t s_rf_manual_power_dbm;
static bool s_rf_policy_apply_pending;
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
static bool s_rf_report_submitting;
static rf_dispatch_cmd_t s_rf_user_intent;
static bool s_rf_user_intent_pending;
static bool s_rf_user_intent_queued;
static bool s_rf_dispatch_active;
static uint32_t s_rf_user_intent_generation;
static uint32_t s_rf_user_intent_started_ms;
static uint32_t s_rf_user_intent_retry_after_ms;
static bool s_rf_background_active;
static rf_dispatch_cmd_t s_rf_background_command;
static uint32_t s_rf_last_negative_notice_ms;
static bool s_rf_terminal_notice_pending;
static uint16_t s_rf_terminal_notice_addr;
static rf_background_publish_status_t s_rf_terminal_notice_status;
#endif
static char s_rf_expected_request_id[17];
static bool s_rf_expected_user_transaction;
static bool s_rf_policy_user_transaction;
static char s_rf_policy_request_id[17];
static portMUX_TYPE s_rf_power_lock = portMUX_INITIALIZER_UNLOCKED;

static bool rf_power_request_id_valid(const char *request_id);
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
static void rf_power_reconcile_pending_intent(void);
static void rf_power_handle_accepted_report_locked(uint32_t now,
    bool matched_transaction);
static void rf_power_clear_background_locked(void);
#endif

static bool rf_power_is_monitored_cluster(uint16_t cluster_id)
{
    return cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT ||
           cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT ||
           cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT ||
           cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF ||
           cluster_id == RF_POWER_CLUSTER_ID;
}

static bool rf_tx_backoff_active_locked(uint32_t now)
{
    return s_rf_tx_backoff_until_ms != 0 &&
           (int32_t)(now - s_rf_tx_backoff_until_ms) < 0;
}

static void rf_tx_clear_backoff_locked(void)
{
    s_rf_tx_fail_count = 0;
    s_rf_tx_backoff_until_ms = 0;
}

static void rf_tx_note_result_locked(esp_err_t err, uint32_t now)
{
    uint32_t delay_ms;

    if (err == ESP_OK) {
        rf_tx_clear_backoff_locked();
        return;
    }

    if (s_rf_tx_fail_count < UINT8_MAX) {
        s_rf_tx_fail_count++;
    }

    if (s_rf_tx_fail_count == 1) {
        delay_ms = RF_TX_BACKOFF_FIRST_MS;
    } else if (s_rf_tx_fail_count == 2) {
        delay_ms = RF_TX_BACKOFF_SECOND_MS;
    } else {
        delay_ms = RF_TX_BACKOFF_MAX_MS;
    }
    s_rf_tx_backoff_until_ms = now + delay_ms;
}


#define RF_POLICY_NVS_NAMESPACE     "rfpol"

static const char *rf_power_mode_name(uint8_t mode)
{
    if (mode == IOT_RF_MODE_MANUAL) {
        return "manual";
    }
    if (mode == IOT_RF_MODE_AUTO) {
        return "auto";
    }
    return "unknown";
}

static bool rf_power_policy_values_valid(uint8_t mode, int8_t manual_power_dbm)
{
    return (mode == IOT_RF_MODE_AUTO || mode == IOT_RF_MODE_MANUAL) &&
           iot_rf_power_policy_value_allowed(&s_rf_power_policy_config, manual_power_dbm);
}

static void rf_power_policy_persist(uint8_t mode, int8_t manual_power_dbm)
{
    nvs_handle_t handle;

    if (nvs_open(RF_POLICY_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "RF power policy persistence namespace unavailable");
        return;
    }

    uint8_t stored_mode = IOT_RF_MODE_UNKNOWN;
    int8_t stored_manual_power = RF_POWER_UNKNOWN_DBM;
    esp_err_t mode_err = nvs_get_u8(handle, "mode", &stored_mode);
    esp_err_t power_err = nvs_get_i8(handle, "mpwr", &stored_manual_power);
    if (mode_err == ESP_OK && power_err == ESP_OK &&
        stored_mode == mode && stored_manual_power == manual_power_dbm) {
        nvs_close(handle);
        ESP_LOGI(TAG, "RF power policy unchanged; skip Flash write");
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

static void rf_policy_bump_generation_locked(void)
{
    ++s_rf_policy_generation;
    if (s_rf_policy_generation == RF_POLICY_GENERATION_ANY) {
        s_rf_policy_generation = 1;
    }
}

void rf_power_controller_init(void)
{
    uint8_t mode = IOT_RF_MODE_AUTO;
    int8_t manual_power_dbm = RF_POWER_AUTO_MIN_DBM;
    nvs_handle_t handle;

    if (nvs_open(RF_POLICY_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t stored_mode = IOT_RF_MODE_AUTO;
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
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    memset(&s_rf_user_intent, 0, sizeof(s_rf_user_intent));
    s_rf_user_intent_pending = false;
    s_rf_user_intent_queued = false;
    s_rf_dispatch_active = false;
    s_rf_user_intent_generation = 0U;
    s_rf_user_intent_started_ms = 0U;
    s_rf_user_intent_retry_after_ms = 0U;
    s_rf_report_submitting = false;
    rf_power_clear_background_locked();
    if (s_rf_policy_generation == 0U ||
        s_rf_policy_generation == RF_POLICY_GENERATION_ANY) {
        s_rf_policy_generation = 1U;
    }
#endif
    ESP_LOGI(TAG, "RF power policy: mode=%s manual=%d dBm",
             rf_power_mode_name(mode), (int)manual_power_dbm);

    // Install the shared log hook used to detect a ZBOSS MAC no-buffer storm.
    rf_health_log_init();
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    ESP_ERROR_CHECK(rf_cq_configure_dispatch(rf_power_dispatch_from_queue));
    ESP_ERROR_CHECK(rf_cq_start());
#endif
}

void rf_power_controller_on_leave(uint16_t addr)
{
    taskENTER_CRITICAL(&s_rf_power_lock);
    if (s_rf_node_addr == addr) {
        s_rf_node_addr = 0;
        s_rf_lqi_ema = -1;
        s_rf_lqi_samples = 0;
        s_rf_last_rx_ms = 0;
        s_rf_node_power_dbm = RF_POWER_UNKNOWN_DBM;
        s_rf_node_mode = IOT_RF_MODE_UNKNOWN;
        s_rf_lqi_low_windows = 0;
        s_rf_lqi_high_windows = 0;
        s_rf_report_waiting = false;
        s_rf_expected_report_seq = 0;
        s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
        s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
        s_rf_last_report_seq_valid = false;
        s_rf_last_report_seq = 0;
        s_rf_wait_started_ms = 0;
        s_rf_power_confirmed_ms = 0;
        rf_tx_clear_backoff_locked();
        s_rf_policy_apply_pending = false;
        s_rf_expected_request_id[0] = '\0';
        s_rf_expected_user_transaction = false;
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        s_rf_report_submitting = false;
        rf_power_clear_background_locked();
        memset(&s_rf_user_intent, 0, sizeof(s_rf_user_intent));
        s_rf_user_intent_pending = false;
        s_rf_user_intent_queued = false;
        s_rf_dispatch_active = false;
        s_rf_user_intent_generation = 0U;
        s_rf_user_intent_started_ms = 0U;
        s_rf_user_intent_retry_after_ms = 0U;
#endif
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);
}

void rf_power_controller_on_announce(uint16_t addr)
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
    s_rf_node_mode = IOT_RF_MODE_UNKNOWN;
    s_rf_lqi_low_windows = 0;
    s_rf_lqi_high_windows = 0;
    s_rf_report_waiting = false;
    s_rf_expected_report_seq = 0;
    s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
    s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
    s_rf_last_report_seq_valid = false;
    s_rf_last_report_seq = 0;
    s_rf_wait_started_ms = 0;
    s_rf_power_confirmed_ms = 0;
    rf_tx_clear_backoff_locked();
    s_rf_policy_apply_pending = s_rf_policy_user_transaction ||
                                 (s_rf_mode == IOT_RF_MODE_MANUAL);
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    if (!s_rf_policy_user_transaction) {
        s_rf_policy_request_id[0] = '\0';
    }
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    s_rf_report_submitting = false;
    rf_power_clear_background_locked();
    s_rf_dispatch_active = false;
    s_rf_user_intent_queued = false;
    if (s_rf_user_intent_pending) {
        s_rf_user_intent_retry_after_ms = 0U;
    }
#endif
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
        if (s_mqtt_publish) { s_mqtt_publish(suffix, payload, true); }
    }
    cJSON_free(payload);
    cJSON_Delete(root);
}

static void rf_power_publish_querying(uint16_t addr, const char *request_id)
{
    rf_power_publish_state(addr, RF_POWER_UNKNOWN_DBM,
                           IOT_RF_MODE_UNKNOWN, "querying", request_id);
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
    s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    s_rf_wait_started_ms = 0;
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    s_rf_report_submitting = false;
#endif
}

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
static bool rf_power_close_background_timeout_if_due_locked(
    uint32_t now, bool *should_publish,
    rf_background_publish_status_t *publish_status);
#endif

// Called while s_rf_power_lock is held. Once the deadline passes, a late,
// otherwise exact REPORT must no longer confirm the original user transaction.
static bool rf_power_expire_waiting_if_due_locked(uint32_t now)
{
    if (!s_rf_report_waiting || s_rf_wait_started_ms == 0 ||
        now - s_rf_wait_started_ms < RF_POWER_SETTLE_MS) {
        return false;
    }

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    {
        bool background_timeout_publish = false;
        rf_background_publish_status_t background_timeout_status =
            RF_BG_STATUS_PUBLISH_TIMEOUT;

        if (rf_power_close_background_timeout_if_due_locked(
                now, &background_timeout_publish,
                &background_timeout_status)) {
            if (background_timeout_publish) {
                s_rf_terminal_notice_pending = true;
                s_rf_terminal_notice_addr = s_rf_node_addr;
                s_rf_terminal_notice_status = background_timeout_status;
            }
            return true;
        }
    }
#endif

    rf_power_clear_waiting_locked();
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    if (s_rf_user_intent_pending) {
        s_rf_user_intent_queued = false;
        s_rf_user_intent_retry_after_ms = 0;
    }
#endif
    return true;
}

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
static void rf_power_clear_background_locked(void)
{
    s_rf_background_active = false;
    memset(&s_rf_background_command, 0, sizeof(s_rf_background_command));
}
static bool rf_power_handle_background_report_locked(
    uint32_t now, int8_t reported_power_dbm, uint8_t reported_mode,
    bool matched_sequence, bool node_initiated_report,
    bool *should_publish,
    rf_background_publish_status_t *publish_status)
{
    rf_background_status_state_t status_state;
    rf_background_status_plan_t status_plan;
    rf_dispatch_cmd_t command;

    if (should_publish == NULL || publish_status == NULL) {
        return false;
    }
    *should_publish = false;
    if (!s_rf_background_active) {
        return true;
    }

    command = s_rf_background_command;
    memset(&status_state, 0, sizeof(status_state));
    status_state.now_ms = now;
    status_state.command = command;
    status_state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    status_state.reported_power_dbm = reported_power_dbm;
    status_state.reported_mode = reported_mode;
    status_state.matched_transaction = matched_sequence;
    status_state.node_initiated_report = node_initiated_report;
    status_state.policy_apply_pending = s_rf_policy_apply_pending;
    status_state.notice_allowed = true;

    if (!rf_background_status_plan_create(&status_state, &status_plan)) {
        return false;
    }
    if (status_plan.result == RF_BG_STATUS_PLAN_INVALID_ARG) {
        return true;
    }

    if (status_plan.result == RF_BG_STATUS_PLAN_PUBLISH) {
        if (status_plan.clear_background) {
            rf_power_clear_background_locked();
        }
        if (status_plan.set_policy_pending) {
            s_rf_policy_apply_pending = status_plan.policy_pending_value;
            if (!status_plan.policy_pending_value) {
                s_rf_policy_request_id[0] = '\0';
            }
        }
        *should_publish = true;
        *publish_status = status_plan.publish_status;
    }

    // NO_ACTION: keep active transaction and do not publish.
    // table_action is descriptive only; gateway has no command DB API.
    return true;
}

#define RF_BG_NEGATIVE_NOTICE_MIN_MS 300000u

static bool rf_power_negative_notice_allowed_locked(uint32_t now)
{
    return s_rf_last_negative_notice_ms == 0U ||
           now - s_rf_last_negative_notice_ms >=
               RF_BG_NEGATIVE_NOTICE_MIN_MS;
}

static bool rf_power_handle_background_terminal_locked(
    uint32_t now, rf_background_status_event_t event,
    bool *should_publish,
    rf_background_publish_status_t *publish_status)
{
    rf_background_status_state_t status_state;
    rf_background_status_plan_t status_plan;

    if (should_publish == NULL || publish_status == NULL) {
        return false;
    }
    *should_publish = false;
    if (!s_rf_background_active) {
        return true;
    }

    memset(&status_state, 0, sizeof(status_state));
    status_state.now_ms = now;
    status_state.command = s_rf_background_command;
    status_state.event = event;
    status_state.reported_power_dbm = RF_POWER_UNKNOWN_DBM;
    status_state.reported_mode = IOT_RF_MODE_UNKNOWN;
    status_state.matched_transaction = false;
    status_state.node_initiated_report = false;
    status_state.policy_apply_pending = s_rf_policy_apply_pending;
    status_state.notice_allowed =
        rf_power_negative_notice_allowed_locked(now);

    if (!rf_background_status_plan_create(&status_state, &status_plan)) {
        return false;
    }
    if (status_plan.result == RF_BG_STATUS_PLAN_INVALID_ARG ||
        status_plan.result == RF_BG_STATUS_PLAN_NO_ACTION) {
        return true;
    }

    if (status_plan.clear_background) {
        rf_power_clear_background_locked();
    }
    if (status_plan.set_policy_pending) {
        s_rf_policy_apply_pending = status_plan.policy_pending_value;
        if (!status_plan.policy_pending_value) {
            s_rf_policy_request_id[0] = '\0';
        }
    }

    if (status_plan.result == RF_BG_STATUS_PLAN_PUBLISH) {
        s_rf_last_negative_notice_ms = now;
        *should_publish = true;
        *publish_status = status_plan.publish_status;
    }
    return true;
}

static bool rf_power_close_background_timeout_if_due_locked(
    uint32_t now, bool *should_publish,
    rf_background_publish_status_t *publish_status)
{
    if (should_publish == NULL || publish_status == NULL) {
        return false;
    }
    *should_publish = false;
    if (!s_rf_report_waiting || s_rf_wait_started_ms == 0U ||
        now - s_rf_wait_started_ms < RF_POWER_SETTLE_MS) {
        return false;
    }
    if (s_rf_expected_user_transaction) {
        return false;
    }

    if (s_rf_background_active) {
        if (s_rf_background_command.policy_generation !=
                s_rf_policy_generation) {
            rf_power_clear_background_locked();
        } else if (!rf_power_handle_background_terminal_locked(
                now, RF_BG_STATUS_EVENT_WAIT_TIMEOUT,
                should_publish, publish_status)) {
            return false;
        } else {
            rf_power_clear_background_locked();
        }
    }
    rf_power_clear_waiting_locked();
    return true;
}

static void rf_power_publish_background_terminal_notice(void)
{
    bool pending = false;
    uint16_t addr = 0U;
    rf_background_publish_status_t status =
        RF_BG_STATUS_PUBLISH_TIMEOUT;

    taskENTER_CRITICAL(&s_rf_power_lock);
    pending = s_rf_terminal_notice_pending;
    if (pending) {
        addr = s_rf_terminal_notice_addr;
        status = s_rf_terminal_notice_status;
        s_rf_terminal_notice_pending = false;
        s_rf_terminal_notice_addr = 0U;
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (pending) {
        const char *status_name = status == RF_BG_STATUS_PUBLISH_FAILED
                                      ? "failed"
                                      : "timeout";
        rf_power_publish_state(addr, RF_POWER_UNKNOWN_DBM,
                               IOT_RF_MODE_UNKNOWN, status_name, NULL);
    }
}


static void rf_power_clear_user_intent_locked(void)
{
    memset(&s_rf_user_intent, 0, sizeof(s_rf_user_intent));
    s_rf_user_intent_pending = false;
    s_rf_user_intent_queued = false;
    s_rf_user_intent_generation = 0U;
    s_rf_user_intent_started_ms = 0U;
    s_rf_user_intent_retry_after_ms = 0U;
    s_rf_report_submitting = false;
}

static void rf_power_arm_user_intent_locked(
    const rf_dispatch_cmd_t *cmd, uint32_t now)
{
    s_rf_user_intent = *cmd;
    s_rf_user_intent_pending = true;
    s_rf_user_intent_queued = false;
    s_rf_dispatch_active = false;
    s_rf_user_intent_generation = s_rf_policy_generation;
    s_rf_user_intent_started_ms = now;
    s_rf_user_intent_retry_after_ms = 0U;
    s_rf_report_submitting = false;
}
#endif

// Runs in Zigbee stack context. Keep it short: only observe the raw APS result.
// Registering this callback also installs the SDK's internal confirm wrapper,
// which releases the sender buffer after this application callback returns.
void rf_power_controller_aps_data_confirm(esp_zb_apsde_data_confirm_t confirm)
{
    ESP_LOGI(TAG,
             "APS data confirm: status=%u dst_mode=%u dst_ep=%u src_ep=%u asdu_len=%lu",
             (unsigned)confirm.status,
             (unsigned)confirm.dst_addr_mode,
             (unsigned)confirm.dst_endpoint,
             (unsigned)confirm.src_endpoint,
             (unsigned long)confirm.asdu_length);
}

bool rf_power_controller_aps_indication(esp_zb_apsde_data_ind_t ind)
{
    bool is_private = ind.cluster_id == RF_POWER_CLUSTER_ID;
    bool power_report = false;
    bool accept_report = false;
    bool packet_v2 = false;
    int8_t reported_power = 0;
    uint8_t reported_mode = IOT_RF_MODE_UNKNOWN;
    uint8_t report_seq = 0;
    uint8_t expected_seq = 0;
    uint8_t expected_mode = IOT_RF_MODE_UNKNOWN;
    int8_t expected_power = RF_POWER_UNKNOWN_DBM;
    uint8_t last_seq = 0;
    bool expected_user = false;
    int8_t recent_rssi = -127;
    bool old_power_unknown = true;
    bool matched_transaction = false;
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    bool matched_background = false;
    bool background_active = false;
    bool background_report_handled = false;
    bool background_publish = false;
    bool background_spontaneous_report = false;
    rf_background_publish_status_t background_publish_status =
        RF_BG_STATUS_PUBLISH_REPORT_RECEIVED;
#endif
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

        iot_rf_packet_t private_packet;

        if (!iot_rf_packet_decode(ind.asdu, ind.asdu_length,
                                  &private_packet)) {
            reject_reason = RF_REJECT_MALFORMED;
            return true;
        }

        packet_v2 = private_packet.version == IOT_RF_PACKET_V2_VERSION;
        report_seq = private_packet.sequence;

        if (private_packet.command != IOT_RF_CMD_REPORT) {
            reject_reason = RF_REJECT_UNEXPECTED_CMD;
            return true;
        }

        reported_power = private_packet.power_dbm;
        power_report = true;
        if (packet_v2) {
            reported_mode = private_packet.mode;
        }
    }

    recent_rssi = esp_ieee802154_get_recent_rssi();
    int frame_lqi = ind.lqi;
    bool active_child = ((s_active_child_check) ? s_active_child_check(ind.src_short_addr) : false);
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
        } else if (!iot_rf_power_policy_value_allowed(&s_rf_power_policy_config, reported_power)) {
            reject_reason = RF_REJECT_INVALID_POWER;
        } else if (s_rf_report_waiting) {
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
            bool background_spontaneous_confirms_set = false;
            bool spontaneous_report_is_new = false;

            if (report_seq >= 0x80) {
                spontaneous_report_is_new =
                    rf_power_report_is_new_unsolicited_locked(report_seq);
                background_spontaneous_confirms_set =
                    spontaneous_report_is_new &&
                    packet_v2 &&
                    !expected_user &&
                    s_rf_background_active &&
                    s_rf_policy_apply_pending &&
                    s_rf_background_command.policy_generation ==
                        s_rf_policy_generation &&
                    s_rf_background_command.command ==
                        RF_CQ_COMMAND_SET &&
                    ((s_rf_background_command.desired_mode ==
                          RF_CQ_MODE_MANUAL &&
                      reported_mode == IOT_RF_MODE_MANUAL &&
                      reported_power ==
                          s_rf_background_command.desired_power_dbm) ||
                     (s_rf_background_command.desired_mode ==
                          RF_CQ_MODE_AUTO &&
                      reported_mode == IOT_RF_MODE_AUTO));
            }
#endif
            const iot_rf_transaction_expected_t expected_report = {
                .sequence = expected_seq, .power_dbm = expected_power,
                .mode = expected_mode,
            };
            const iot_rf_transaction_report_t received_report = {
                .sequence = report_seq, .power_dbm = reported_power,
                .mode = reported_mode,
            };
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
            if (background_spontaneous_confirms_set) {
                accept_report = true;
                matched_transaction = false;
                matched_background = true;
                background_spontaneous_report = true;
            } else
#endif
            if (packet_v2 &&
                iot_rf_transaction_report_matches(&expected_report,
                                                  &received_report)) {
                accept_report = true;
                matched_transaction = expected_user;
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
                matched_background = !expected_user &&
                    s_rf_background_active &&
                    s_rf_background_command.policy_generation ==
                        s_rf_policy_generation;
#endif
                strlcpy(response_request_id, s_rf_expected_request_id,
                        sizeof(response_request_id));
                s_rf_expected_request_id[0] = '\0';
                s_rf_report_waiting = false;
                s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
                s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
                s_rf_wait_started_ms = 0;
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
            } else if (report_seq >= 0x80 && !spontaneous_report_is_new) {
                reject_reason = RF_REJECT_OLD;
#endif
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
        s_rf_node_mode = packet_v2 ? reported_mode : IOT_RF_MODE_UNKNOWN;
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        background_active = s_rf_background_active;
        background_report_handled =
            rf_power_handle_background_report_locked(
                now, reported_power, reported_mode, matched_background,
                report_seq >= 0x80, &background_publish,
                &background_publish_status);
        if (!background_report_handled) {
            ESP_LOGW(TAG, "RF power: background report plan failed");
        } else if (background_spontaneous_report &&
                   background_publish &&
                   background_publish_status ==
                       RF_BG_STATUS_PUBLISH_CONFIRMED) {
            rf_power_clear_waiting_locked();
        }
#endif
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        if (!background_active && !s_rf_policy_user_transaction && packet_v2 &&
#else
        if (!s_rf_policy_user_transaction && packet_v2 &&
#endif
            ((s_rf_mode == IOT_RF_MODE_MANUAL &&
              reported_mode == IOT_RF_MODE_MANUAL &&
              reported_power == s_rf_manual_power_dbm) ||
             (s_rf_mode == IOT_RF_MODE_AUTO &&
              reported_mode == IOT_RF_MODE_AUTO))) {
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
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        rf_power_handle_accepted_report_locked(now, matched_transaction);
#endif
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
        const char *publish_status_name =
            matched_transaction ? "confirmed" : "report_received";
        const char *publish_request_id =
            matched_transaction ? response_request_id : NULL;

        ESP_LOGI(TAG,
                 "RF power: report accepted from %s to %s/%d dBm seq=%u",
                 old_power_unknown ? "unknown" : "previous",
                 rf_power_mode_name(reported_mode),
                 (int)reported_power, report_seq);
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        if (background_active) {
            if (!background_report_handled || !background_publish) {
                publish_status_name = NULL;
                ESP_LOGI(TAG,
                         "RF power: background report publication suppressed");
            } else {
                publish_status_name =
                    background_publish_status ==
                            RF_BG_STATUS_PUBLISH_CONFIRMED
                        ? "confirmed"
                        : "report_received";
                publish_request_id = NULL;
            }
        }
#endif
        if (publish_status_name != NULL) {
            rf_power_publish_state(ind.src_short_addr, reported_power,
                                   reported_mode, publish_status_name,
                                   publish_request_id);
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
            if (publish_request_id != NULL) {
                ESP_LOGI(TAG, "LOCAL_RF_COMPLETE request_id=%s",
                         publish_request_id);
            }
#endif
        }
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
                                       uint8_t mode, uint8_t *sent_seq,
                                       uint32_t expected_generation)
{
    uint8_t seq = 0;
    uint16_t addr = 0;
    bool generation_stale = false;

    if (mode != IOT_RF_MODE_AUTO && mode != IOT_RF_MODE_MANUAL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    if (expected_generation != RF_POLICY_GENERATION_ANY &&
        expected_generation != s_rf_policy_generation) {
        generation_stale = true;
    } else {
        addr = s_rf_node_addr;
        seq = ++s_rf_packet_sequence;
        if (seq == 0 || seq >= 0x80) {
            seq = 1;
            s_rf_packet_sequence = 1;
        }
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (generation_stale) {
        ESP_LOGW(TAG, "RF power -> skip stale periodic dispatch generation=%lu",
                 (unsigned long)expected_generation);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t asdu[IOT_RF_PACKET_V2_LEN];
    const iot_rf_packet_t outgoing_packet = {
        .version = IOT_RF_PACKET_V2_VERSION,
        .command = command,
        .sequence = seq,
        .power_dbm = power_dbm,
        .mode = mode,
    };
    size_t encoded_len = 0U;

    if (!iot_rf_packet_encode(&outgoing_packet, asdu, sizeof(asdu),
                              &encoded_len) ||
        encoded_len != IOT_RF_PACKET_V2_LEN) {
        return ESP_FAIL;
    }
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
    esp_err_t err;

    // This function runs in the forwarding/status task, not inside a Zigbee
    // callback. Serialize SDK access to avoid desynchronizing the port-layer
    // receive/transmit state.
    if (!esp_zb_is_started()) {
        ESP_LOGW(TAG, "RF power -> node=0x%04x seq=%u skipped: stack not started",
                 (unsigned)addr, seq);
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        ESP_LOGW(TAG, "RF power -> node=0x%04x seq=%u skipped: lock unavailable",
                 (unsigned)addr, seq);
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_zb_aps_data_request(&req);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "RF power -> node=0x%04x cmd=%u power=%d mode=%s seq=%u: %s",
             (unsigned)addr, command, power_dbm, rf_power_mode_name(mode),
             seq, esp_err_to_name(err));

    bool was_backoff;
    uint8_t tx_fail_count;
    uint32_t tx_backoff_until;

    taskENTER_CRITICAL(&s_rf_power_lock);
    uint32_t state_now = pdTICKS_TO_MS(xTaskGetTickCount());
    was_backoff = rf_tx_backoff_active_locked(state_now);
    rf_tx_note_result_locked(err, state_now);
    if (err != ESP_OK) {
        s_rf_expected_request_id[0] = '\0';
    }
    tx_fail_count = s_rf_tx_fail_count;
    tx_backoff_until = s_rf_tx_backoff_until_ms;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (err == ESP_OK) {
        if (was_backoff) {
            ESP_LOGI(TAG, "RF tx backoff cleared");
        }
    } else {
        ESP_LOGI(TAG,
                 "RF tx backoff armed: failures=%u until_ms=%lu",
                 (unsigned)tx_fail_count,
                 (unsigned long)tx_backoff_until);
    }

    if (err == ESP_OK && sent_seq != NULL) {
        *sent_seq = seq;
    }
    return err;
}

#if !defined(RF_COMMAND_QUEUE_ENABLED) || !RF_COMMAND_QUEUE_ENABLED

void rf_power_controller_mqtt_query(const char *request_id)
{
    uint16_t addr = 0;
    bool report_waiting = false;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    taskENTER_CRITICAL(&s_rf_power_lock);
    // A user QUERY may take over an automatic transaction whose wait deadline
    // has already passed. Otherwise a failed periodic retry could leave a stale
    // waiting flag and block the user for the whole backoff window.
    (void)rf_power_expire_waiting_if_due_locked(now);
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
    rf_policy_bump_generation_locked();
    taskEXIT_CRITICAL(&s_rf_power_lock);
    request_id = s_rf_expected_request_id[0] != '\0' ? s_rf_expected_request_id : NULL;

    uint8_t sent_seq = 0;
    rf_power_publish_querying(addr, request_id);
    if (rf_power_send_to_node(IOT_RF_CMD_QUERY, 0, IOT_RF_MODE_AUTO, &sent_seq,
                                  RF_POLICY_GENERATION_ANY) == ESP_OK) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
        s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
        s_rf_expected_user_transaction = true;
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    } else {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_expected_user_transaction = false;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}

void rf_power_controller_mqtt_set_manual(int8_t target_power_dbm, const char *request_id)
{
    uint16_t addr = 0;
    int8_t current_power_dbm = RF_POWER_UNKNOWN_DBM;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    if (!iot_rf_power_policy_value_allowed(&s_rf_power_policy_config, target_power_dbm)) {
        ESP_LOGW(TAG, "RF power: reject remote target=%d dBm", (int)target_power_dbm);
        return;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_mode = IOT_RF_MODE_MANUAL;
    s_rf_manual_power_dbm = target_power_dbm;
    s_rf_policy_apply_pending = true;
    s_rf_report_waiting = false;
    s_rf_expected_report_seq = 0;
    s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
    s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    s_rf_wait_started_ms = 0;
    /* The known-node path sends below; retain the periodic cooldown. The
       generation check below also blocks a periodic snapshot overtaken by this
       command before it can reserve a second Zigbee sequence. */
    s_rf_last_eval_ms = now;
    s_rf_policy_user_transaction = true;
    rf_set_policy_request_id_locked(request_id);
    rf_policy_bump_generation_locked();
    addr = s_rf_node_addr;
    current_power_dbm = s_rf_node_power_dbm;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_policy_persist(IOT_RF_MODE_MANUAL, target_power_dbm);

    if (addr == 0) {
        ESP_LOGI(TAG, "RF power: manual policy saved; apply when node joins");
        return;
    }
    if (current_power_dbm == RF_POWER_UNKNOWN_DBM) {
        uint8_t sent_seq = 0;
        if (rf_power_send_to_node(IOT_RF_CMD_QUERY, 0, IOT_RF_MODE_AUTO, &sent_seq,
                                  RF_POLICY_GENERATION_ANY) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
            s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
            s_rf_expected_user_transaction = false;
            s_rf_expected_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }

    uint8_t sent_seq = 0;
    if (rf_power_send_to_node(IOT_RF_CMD_SET, target_power_dbm, IOT_RF_MODE_MANUAL, &sent_seq,
                              RF_POLICY_GENERATION_ANY) == ESP_OK) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = target_power_dbm;
        s_rf_expected_report_mode = IOT_RF_MODE_MANUAL;
        s_rf_expected_user_transaction = s_rf_policy_user_transaction;
        strlcpy(s_rf_expected_request_id, s_rf_policy_request_id, sizeof(s_rf_expected_request_id));
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}

void rf_power_controller_mqtt_set_auto(const char *request_id)
{
    uint16_t addr = 0;
    int8_t current_power_dbm = RF_POWER_UNKNOWN_DBM;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_mode = IOT_RF_MODE_AUTO;
    s_rf_policy_apply_pending = true;
    s_rf_report_waiting = false;
    s_rf_expected_report_seq = 0;
    s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
    s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
    s_rf_wait_started_ms = 0;
    /* The known-node path sends below; retain the periodic cooldown. The
       generation check below also blocks a periodic snapshot overtaken by this
       command before it can reserve a second Zigbee sequence. */
    s_rf_last_eval_ms = now;
    s_rf_policy_user_transaction = true;
    rf_set_policy_request_id_locked(request_id);
    rf_policy_bump_generation_locked();
    addr = s_rf_node_addr;
    current_power_dbm = s_rf_node_power_dbm;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_policy_persist(IOT_RF_MODE_AUTO, s_rf_manual_power_dbm);
    ESP_LOGI(TAG, "RF power: automatic policy enabled");

    if (addr == 0) {
        ESP_LOGI(TAG, "RF power: automatic policy saved; apply when node joins");
        return;
    }
    if (current_power_dbm == RF_POWER_UNKNOWN_DBM) {
        uint8_t sent_seq = 0;
        if (rf_power_send_to_node(IOT_RF_CMD_QUERY, 0, IOT_RF_MODE_AUTO, &sent_seq,
                                  RF_POLICY_GENERATION_ANY) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
            s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
            s_rf_expected_user_transaction = false;
            s_rf_expected_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }

    uint8_t sent_seq = 0;
    if (rf_power_send_to_node(IOT_RF_CMD_SET, current_power_dbm, IOT_RF_MODE_AUTO, &sent_seq,
                              RF_POLICY_GENERATION_ANY) == ESP_OK) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = current_power_dbm;
        s_rf_expected_report_mode = IOT_RF_MODE_AUTO;
        s_rf_expected_user_transaction = s_rf_policy_user_transaction;
        strlcpy(s_rf_expected_request_id, s_rf_policy_request_id, sizeof(s_rf_expected_request_id));
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}

#endif

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED

// Bounds one user-facing SET attempt. The desired MANUAL/AUTO policy remains
// persisted after this timeout; later CQ-3 background dispatch can take it over.
#define RF_DISPATCH_SET_INTENT_TTL_MS 1800000

static uint32_t rf_power_new_command_id(void)
{
    uint32_t id = 0U;

    do {
        id = esp_random();
    } while (id == 0U);
    return id;
}

static uint32_t rf_power_new_background_command_id(uint32_t active_id)
{
    uint32_t id = esp_random();

    if (id == 0U) {
        id = 1U;
    }
    if (id == active_id) {
        id = active_id == UINT32_MAX ? 1U : active_id + 1U;
    }
    return id;
}

static bool rf_power_map_runtime_locked(
    rf_background_runtime_state_t *runtime, uint32_t now)
{
    rf_background_command_mapper_input_t input;
    rf_background_command_mapper_output_t output;

    memset(&input, 0, sizeof(input));
    input.now_ms = now;
    input.last_eval_ms = s_rf_last_eval_ms;
    input.node_addr = s_rf_node_addr;
    input.last_rx_ms = s_rf_last_rx_ms;
    input.confirmed_ms = s_rf_power_confirmed_ms;
    input.current_power_dbm = s_rf_node_power_dbm;
    input.node_mode = s_rf_node_mode;
    input.desired_mode = s_rf_mode;
    input.desired_manual_power_dbm = s_rf_manual_power_dbm;
    input.policy_apply_pending = s_rf_policy_apply_pending;
    input.tx_backoff_until_ms = s_rf_tx_backoff_until_ms;
    input.user_intent_active = s_rf_user_intent_pending;
    input.report_waiting = s_rf_report_waiting;
    input.report_submitting = s_rf_report_submitting;
    input.wait_started_ms = s_rf_wait_started_ms;
    input.dispatch_active = s_rf_dispatch_active;
    input.background_command_active = s_rf_background_active;
    input.active_command_id = s_rf_background_command.id;
    input.active_command_generation = s_rf_background_command.policy_generation;
    input.next_command_id =
        rf_power_new_background_command_id(s_rf_background_command.id);
    input.policy_generation = s_rf_policy_generation;

    if (!rf_background_command_mapper_map(&input, &output) ||
        output.result != RF_BG_MAPPER_OK) {
        return false;
    }

    *runtime = output.runtime;
    return true;
}

static void rf_power_apply_runtime_locked(
    const rf_background_runtime_state_t *runtime)
{
    s_rf_last_eval_ms = runtime->last_eval_ms;
    s_rf_node_power_dbm = runtime->current_power_dbm;
    s_rf_node_mode = runtime->node_mode;
    s_rf_power_confirmed_ms = runtime->confirmed_ms;
    s_rf_policy_apply_pending = runtime->policy_apply_pending;
    s_rf_report_waiting = runtime->report_waiting;
    s_rf_report_submitting = runtime->report_submitting;
    s_rf_wait_started_ms = runtime->wait_started_ms;

    if (runtime->background_command_active) {
        s_rf_background_active = true;
        s_rf_background_command.id = runtime->active_command_id;
        s_rf_background_command.policy_generation =
            runtime->active_command_generation;
    } else {
        rf_power_clear_background_locked();
    }
}

static void rf_power_background_controller_periodic(void)
{
    rf_background_runtime_state_t runtime;
    rf_background_runtime_adapter_result_t result;
    rf_dispatch_cmd_t command;
    uint32_t now;
    bool should_submit = false;
    esp_err_t submit_err;

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    {
        bool timeout_publish = false;
        rf_background_publish_status_t timeout_status =
            RF_BG_STATUS_PUBLISH_TIMEOUT;
        uint16_t timeout_addr = s_rf_node_addr;

        if (rf_power_close_background_timeout_if_due_locked(
                now, &timeout_publish, &timeout_status)) {
            taskEXIT_CRITICAL(&s_rf_power_lock);
            if (timeout_publish) {
                rf_power_publish_state(
                    timeout_addr, RF_POWER_UNKNOWN_DBM,
                    IOT_RF_MODE_UNKNOWN, "timeout", NULL);
            }
            return;
        }
    }
    if (!rf_power_map_runtime_locked(&runtime, now) ||
        !rf_background_runtime_adapter_apply(&runtime, &result) ||
        result.result == RF_BG_EXEC_INVALID_ARG ||
        result.result == RF_BG_EXEC_REJECTED) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    if (result.state_changed) {
        rf_power_apply_runtime_locked(&result.next_state);
    }
    if (result.has_command) {
        s_rf_background_command = result.command;
        command = result.command;
        should_submit = true;
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (!should_submit) {
        return;
    }

    submit_err = rf_cq_submit(&command);
    if (submit_err == ESP_OK) {
        return;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (rf_power_map_runtime_locked(&runtime, now)) {
        rf_background_runtime_state_t rollback_state;
        rf_background_runtime_rollback_result_t rollback_result;

        if (rf_background_runtime_rollback_command(
                &runtime, &command, &rollback_result, &rollback_state) &&
            rollback_result == RF_BG_RUNTIME_ROLLBACK_DONE) {
            rf_power_apply_runtime_locked(&rollback_state);
        }
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);
}

static void rf_power_generate_cq_request_id(char *buffer)
{
    static const char hex[] = "0123456789abcdef";
    uint64_t value = ((uint64_t)esp_random() << 32) |
                     (uint64_t)esp_random();
    int i;

    for (i = RF_CQ_REQUEST_ID_LEN - 1; i >= 0; --i) {
        buffer[i] = hex[value & 0xfU];
        value >>= 4;
    }
    buffer[RF_CQ_REQUEST_ID_LEN] = '\0';
}

static const char *rf_power_normalize_cq_request_id(
    char *buffer, const char *request_id)
{
    if (rf_cq_request_id_valid(request_id)) {
        return request_id;
    }

    rf_power_generate_cq_request_id(buffer);
    return buffer;
}

static esp_err_t rf_power_execute_plan(const rf_dispatch_plan_t *plan)
{
    uint8_t asdu[IOT_RF_PACKET_V2_LEN];
    const iot_rf_packet_t outgoing_packet = {
        .version = IOT_RF_PACKET_V2_VERSION,
        .command = plan->command,
        .sequence = plan->seq,
        .power_dbm = plan->power_dbm,
        .mode = plan->mode,
    };
    size_t encoded_len = 0U;

    if (!iot_rf_packet_encode(&outgoing_packet, asdu, sizeof(asdu),
                              &encoded_len) ||
        encoded_len != IOT_RF_PACKET_V2_LEN) {
        return ESP_FAIL;
    }
    esp_zb_apsde_data_req_t req = {
        .dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .dst_addr.addr_short = plan->node_addr,
        .dst_endpoint = RF_ZB_ENDPOINT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = RF_POWER_CLUSTER_ID,
        .src_endpoint = RF_ZB_ENDPOINT,
        .asdu_length = sizeof(asdu),
        .asdu = asdu,
        .tx_options = ESP_ZB_APSDE_TX_OPT_SECURITY_ENABLED |
                       ESP_ZB_APSDE_TX_OPT_ACK_TX,
        .use_alias = false,
        .alias_src_addr = 0,
        .alias_seq_num = 0,
        .radius = 10,
    };
    esp_err_t err;

    if (!esp_zb_is_started()) {
        ESP_LOGW(TAG, "RF power -> node=0x%04x seq=%u skipped: stack not started",
                 (unsigned)plan->node_addr, plan->seq);
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        ESP_LOGW(TAG, "RF power -> node=0x%04x seq=%u skipped: lock unavailable",
                 (unsigned)plan->node_addr, plan->seq);
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_zb_aps_data_request(&req);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "RF power -> node=0x%04x cmd=%u power=%d mode=%s seq=%u: %s",
             (unsigned)plan->node_addr, plan->command, plan->power_dbm,
             rf_power_mode_name(plan->mode), plan->seq, esp_err_to_name(err));
    return err;
}

static void rf_power_prepare_manual_policy_locked(
    int8_t target_power_dbm, const char *request_id, uint32_t now)
{
    rf_power_clear_waiting_locked();
    rf_power_clear_background_locked();
    s_rf_mode = IOT_RF_MODE_MANUAL;
    s_rf_manual_power_dbm = target_power_dbm;
    s_rf_policy_apply_pending = true;
    s_rf_last_eval_ms = now;
    s_rf_policy_user_transaction = true;
    rf_set_policy_request_id_locked(request_id);
    rf_policy_bump_generation_locked();
}

static void rf_power_prepare_auto_policy_locked(
    const char *request_id, uint32_t now)
{
    rf_power_clear_waiting_locked();
    rf_power_clear_background_locked();
    s_rf_mode = IOT_RF_MODE_AUTO;
    s_rf_policy_apply_pending = true;
    s_rf_last_eval_ms = now;
    s_rf_policy_user_transaction = true;
    rf_set_policy_request_id_locked(request_id);
    rf_policy_bump_generation_locked();
}

static void rf_power_preinstall_plan_waiting_locked(
    const rf_dispatch_cmd_t *cmd, const rf_dispatch_plan_t *plan, uint32_t now)
{
    bool proxy_query = cmd->command == RF_CQ_COMMAND_SET &&
                       plan->command == RF_DISPATCH_COMMAND_QUERY;

    s_rf_report_waiting = true;
    s_rf_report_submitting = true;
    s_rf_expected_report_seq = plan->seq;
    s_rf_expected_report_power = plan->expected_report_power_dbm;
    s_rf_expected_report_mode = plan->expected_report_mode;
    s_rf_wait_started_ms = now;

    if (proxy_query) {
        s_rf_expected_user_transaction = false;
        s_rf_expected_request_id[0] = '\0';
    } else {
        s_rf_expected_user_transaction = true;
        rf_power_set_request_id_locked(cmd->request_id);
    }
}

static void rf_power_handle_accepted_report_locked(
    uint32_t now, bool matched_transaction)
{
    rf_dispatch_after_report_state_t after_state;
    rf_dispatch_after_report_result_t after_result =
        RF_DISPATCH_AFTER_REPORT_NONE;

    (void)now;
    if (!s_rf_user_intent_pending) {
        return;
    }

    after_state.intent_command = s_rf_user_intent.command;
    if (s_rf_user_intent.command == RF_CQ_COMMAND_SET) {
        after_state.desired_mode = s_rf_user_intent.desired_mode;
        after_state.target_power_dbm = s_rf_user_intent.desired_power_dbm;
    } else {
        after_state.desired_mode = RF_DISPATCH_MODE_AUTO;
        after_state.target_power_dbm = 0;
    }
    after_state.policy_apply_pending = s_rf_policy_apply_pending;
    after_state.reported_power_dbm = s_rf_node_power_dbm;
    after_state.reported_mode = s_rf_node_mode;
    after_state.matched_transaction = matched_transaction;
    after_state.node_initiated_report =
        s_rf_last_report_seq >= 0x80;

    if (!rf_dispatch_after_report_create(&after_state, &after_result)) {
        return;
    }

    if (after_result == RF_DISPATCH_AFTER_REPORT_COMPLETE) {
        if (s_rf_user_intent.command == RF_CQ_COMMAND_SET) {
            s_rf_policy_apply_pending = false;
            s_rf_policy_user_transaction = false;
            s_rf_policy_request_id[0] = '\0';
        }
        rf_power_clear_user_intent_locked();
    } else if (after_result == RF_DISPATCH_AFTER_REPORT_RETRY) {
        s_rf_user_intent_queued = false;
        s_rf_user_intent_retry_after_ms = 0U;
    }
}

static void rf_power_send_intent_command(const rf_dispatch_cmd_t *cmd)
{
    rf_dispatch_plan_state_t state;
    rf_dispatch_plan_t plan;
    uint32_t now;
    bool created = false;
    bool intent_still_matches = false;
    esp_err_t err;

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    (void)rf_power_expire_waiting_if_due_locked(now);

    if (!s_rf_user_intent_pending ||
        s_rf_user_intent.id != cmd->id ||
        s_rf_user_intent_generation != s_rf_policy_generation) {
        s_rf_dispatch_active = false;
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    state.now_ms = now;
    state.node_addr = s_rf_node_addr;
    state.node_power_dbm = s_rf_node_power_dbm;
    state.report_waiting = s_rf_report_waiting;
    state.wait_deadline_ms =
        s_rf_wait_started_ms == 0U
            ? 0U
            : s_rf_wait_started_ms + RF_POWER_SETTLE_MS;
    state.packet_sequence = s_rf_packet_sequence;
    created = rf_dispatch_plan_create(cmd, &state, &plan);

    if (!created) {
        s_rf_dispatch_active = false;
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    if (plan.status != RF_DISPATCH_PLAN_SEND) {
        s_rf_dispatch_active = false;
        if (plan.status == RF_DISPATCH_PLAN_NO_SEND_POLICY_SAVED ||
            plan.status == RF_DISPATCH_PLAN_ERR_INACTIVE) {
            s_rf_user_intent_retry_after_ms = now + RF_POWER_EVAL_MS;
        } else if (plan.status == RF_DISPATCH_PLAN_ERR_WAITING) {
            s_rf_user_intent_retry_after_ms = state.wait_deadline_ms;
        } else {
            rf_power_clear_user_intent_locked();
        }
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    s_rf_packet_sequence = plan.seq;
    rf_power_preinstall_plan_waiting_locked(cmd, &plan, now);
    taskEXIT_CRITICAL(&s_rf_power_lock);

    err = rf_power_execute_plan(&plan);

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    rf_tx_note_result_locked(err, now);
    intent_still_matches =
        s_rf_user_intent_pending &&
        s_rf_user_intent.id == cmd->id &&
        s_rf_user_intent_generation == s_rf_policy_generation;

    // A newer user command can overwrite the intent while the previous APS
    // request is outside the state lock. Completion/failure of that old request
    // must not clear the newer command's queue state or inherit its backoff.
    if (intent_still_matches) {
        if (err != ESP_OK) {
            if (s_rf_report_waiting && s_rf_report_submitting) {
                rf_power_clear_waiting_locked();
            }
            s_rf_report_submitting = false;
            s_rf_user_intent_queued = false;
            s_rf_user_intent_retry_after_ms = s_rf_tx_backoff_until_ms;
        } else if (s_rf_report_submitting) {
            s_rf_report_submitting = false;
            if (!s_rf_report_waiting) {
                s_rf_user_intent_retry_after_ms = 0U;
            }
        }
    }
    s_rf_dispatch_active = false;
    taskEXIT_CRITICAL(&s_rf_power_lock);
}

static void rf_power_preinstall_background_waiting_locked(
    const rf_background_dispatch_plan_t *bg, uint32_t now)
{
    s_rf_report_waiting = true;
    s_rf_report_submitting = true;
    s_rf_expected_report_seq = bg->seq;
    s_rf_expected_report_power = bg->expected_report_power_dbm;
    s_rf_expected_report_mode = bg->expected_report_mode;
    s_rf_wait_started_ms = now;
    s_rf_expected_user_transaction = false;
    s_rf_expected_request_id[0] = '\0';
}

static void rf_power_send_background_command(const rf_dispatch_cmd_t *cmd)
{
    rf_background_dispatch_state_t state;
    rf_background_dispatch_plan_t bg;
    rf_dispatch_plan_t packet;
    uint32_t now;
    bool can_send = false;
    bool terminal_publish = false;
    uint16_t terminal_addr = 0U;
    rf_background_publish_status_t terminal_status =
        RF_BG_STATUS_PUBLISH_FAILED;
    esp_err_t err = ESP_FAIL;

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    memset(&state, 0, sizeof(state));
    state.now_ms = now;
    state.node_addr = s_rf_node_addr;
    state.packet_sequence = s_rf_packet_sequence;
    state.active_command_id = s_rf_background_command.id;
    state.active_command_generation = s_rf_background_command.policy_generation;
    state.user_intent_active = s_rf_user_intent_pending;
    state.report_waiting = s_rf_report_waiting;
    state.report_submitting = s_rf_report_submitting;
    state.dispatch_active = s_rf_dispatch_active;

    if (!rf_background_dispatch_plan_create(cmd, &state, &bg)) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    if (bg.status == RF_BG_DISPATCH_SEND) {
        memset(&packet, 0, sizeof(packet));
        packet.status = RF_DISPATCH_PLAN_SEND;
        packet.node_addr = bg.node_addr;
        packet.seq = bg.seq;
        packet.command = bg.command;
        packet.power_dbm = bg.packet_power_dbm;
        packet.mode = bg.packet_mode;
        packet.publish_querying = false;
        packet.clear_waiting = false;
        packet.expected_report_power_dbm = bg.expected_report_power_dbm;
        packet.expected_report_mode = bg.expected_report_mode;

        s_rf_packet_sequence = bg.seq;
        rf_power_preinstall_background_waiting_locked(&bg, now);
        s_rf_dispatch_active = true;
        can_send = true;
    } else if (bg.status == RF_BG_DISPATCH_CLOSE_EXPIRED ||
               bg.status == RF_BG_DISPATCH_CLOSE_STATE_CHANGED ||
               bg.status == RF_BG_DISPATCH_CLOSE_INACTIVE) {
        rf_power_clear_background_locked();
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (!can_send) {
        return;
    }

    err = rf_power_execute_plan(&packet);

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    rf_tx_note_result_locked(err, now);

    if (s_rf_background_active &&
        s_rf_background_command.policy_generation == s_rf_policy_generation &&
        s_rf_background_command.id == cmd->id &&
        s_rf_background_command.policy_generation == cmd->policy_generation) {
        if (err != ESP_OK) {
            bool should_publish_failure = false;
            rf_background_publish_status_t failure_status =
                RF_BG_STATUS_PUBLISH_FAILED;

            if (rf_power_handle_background_terminal_locked(
                    now, RF_BG_STATUS_EVENT_SEND_FAILED,
                    &should_publish_failure,
                    &failure_status)) {
                terminal_publish = should_publish_failure;
                terminal_status = failure_status;
                terminal_addr = s_rf_node_addr;
            }
            rf_power_clear_background_locked();
            rf_power_clear_waiting_locked();
        } else {
            s_rf_report_submitting = false;
        }
    }
    s_rf_dispatch_active = false;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (terminal_publish) {
        const char *status_name = terminal_status ==
                                      RF_BG_STATUS_PUBLISH_TIMEOUT
                                      ? "timeout"
                                      : "failed";
        rf_power_publish_state(terminal_addr, RF_POWER_UNKNOWN_DBM,
                               IOT_RF_MODE_UNKNOWN, status_name, NULL);
    }
}
static void rf_power_dispatch_from_queue(const rf_dispatch_cmd_t *cmd)
{
    if (cmd->source == RF_CQ_SOURCE_PERIODIC) {
        rf_power_send_background_command(cmd);
        return;
    }

    uint32_t now;
    bool matched = false;
    bool expired = false;

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    matched = s_rf_user_intent_pending &&
              s_rf_user_intent.id == cmd->id &&
              s_rf_user_intent_generation == s_rf_policy_generation;

    if (!matched) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    expired = rf_cq_time_reached(now, cmd->deadline_ms);
    if (expired && cmd->command == RF_CQ_COMMAND_QUERY) {
        rf_power_clear_user_intent_locked();
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    s_rf_user_intent_queued = false;
    s_rf_dispatch_active = true;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (expired) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_dispatch_active = false;
        if (s_rf_user_intent_pending &&
            s_rf_user_intent_generation == s_rf_policy_generation) {
            s_rf_user_intent_retry_after_ms = 0U;
        }
        taskEXIT_CRITICAL(&s_rf_power_lock);
        rf_power_reconcile_pending_intent();
        return;
    }

    rf_power_send_intent_command(cmd);
}

static void rf_power_submit_new_intent(const rf_dispatch_cmd_t *cmd)
{
    esp_err_t err = rf_cq_submit(cmd);
    uint32_t now;

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (err == ESP_OK &&
        s_rf_user_intent_pending &&
        s_rf_user_intent.id == cmd->id &&
        s_rf_user_intent_generation == s_rf_policy_generation) {
        s_rf_user_intent = *cmd;
        s_rf_user_intent_queued = true;
    } else {
        s_rf_user_intent_queued = false;
        s_rf_user_intent_retry_after_ms = now + 100U;
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);
}

static bool rf_power_queued_query(const char *request_id)
{
    rf_dispatch_cmd_t cmd;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    uint32_t generation;

    taskENTER_CRITICAL(&s_rf_power_lock);
    generation = s_rf_policy_generation;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (rf_cq_build_user_query(rf_power_new_command_id(), request_id, now,
                               &cmd) != RF_CQ_OK) {
        ESP_LOGW(TAG, "RF power: reject queued QUERY");
        return false;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (generation != s_rf_policy_generation) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return false;
    }
    // A new QUERY may take over only after the previous waiting transaction is
    // actually due. Otherwise the old command's REPORT could be misattributed
    // to this not-yet-sent QUERY.
    if (s_rf_report_waiting &&
        !rf_power_expire_waiting_if_due_locked(now)) {
        ESP_LOGW(TAG, "RF power: QUERY ignored while another report is pending");
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return false;
    }
    rf_power_arm_user_intent_locked(&cmd, now);
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_submit_new_intent(&cmd);
    return true;
}

static bool rf_power_queued_set_manual(
    int8_t target_power_dbm, const char *request_id)
{
    rf_dispatch_cmd_t cmd;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    uint32_t generation;

    if (!iot_rf_power_policy_value_allowed(&s_rf_power_policy_config, target_power_dbm)) {
        ESP_LOGW(TAG, "RF power: reject queued target=%d dBm",
                 (int)target_power_dbm);
        return false;
    }
    if (rf_cq_build_user_manual_set(rf_power_new_command_id(),
                                   target_power_dbm, request_id, now,
                                   &cmd) != RF_CQ_OK) {
        ESP_LOGW(TAG, "RF power: reject queued MANUAL SET");
        return false;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    rf_power_prepare_manual_policy_locked(target_power_dbm, request_id, now);
    generation = s_rf_policy_generation;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_policy_persist(IOT_RF_MODE_MANUAL, target_power_dbm);

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (generation != s_rf_policy_generation) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return false;
    }
    rf_power_arm_user_intent_locked(&cmd, now);
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_submit_new_intent(&cmd);
    return true;
}

static void rf_power_reconcile_pending_intent(void)
{
    rf_dispatch_cmd_t intent_copy;
    rf_dispatch_cmd_t rebuilt;
    rf_dispatch_plan_state_t state;
    rf_dispatch_plan_t plan;
    uint32_t now;
    uint32_t generation;
    uint32_t started;
    bool created = false;
    bool should_submit = false;
    rf_cq_result_t build_result;
    esp_err_t submit_err;

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    (void)rf_power_expire_waiting_if_due_locked(now);

    if (!s_rf_user_intent_pending ||
        s_rf_user_intent_queued ||
        s_rf_report_waiting ||
        s_rf_report_submitting ||
        s_rf_dispatch_active) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }
    if (s_rf_user_intent_generation != s_rf_policy_generation) {
        rf_power_clear_user_intent_locked();
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }
    if ((int32_t)(now - s_rf_user_intent_retry_after_ms) < 0) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }
    if (s_rf_user_intent.command == RF_CQ_COMMAND_QUERY &&
        now - s_rf_user_intent_started_ms >= RF_CQ_USER_TTL_MS) {
        rf_power_clear_user_intent_locked();
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }
    if (s_rf_user_intent.command == RF_CQ_COMMAND_SET &&
        now - s_rf_user_intent_started_ms >= RF_DISPATCH_SET_INTENT_TTL_MS) {
        ESP_LOGW(TAG,
                 "RF power queue: SET user intent timeout after %lu ms; policy remains saved",
                 (unsigned long)RF_DISPATCH_SET_INTENT_TTL_MS);
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
        rf_power_clear_user_intent_locked();
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    intent_copy = s_rf_user_intent;
    generation = s_rf_user_intent_generation;
    started = s_rf_user_intent_started_ms;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (intent_copy.command == RF_CQ_COMMAND_QUERY) {
        build_result = rf_cq_build_user_query(
            rf_power_new_command_id(), intent_copy.request_id, now, &rebuilt);
    } else if (intent_copy.desired_mode == RF_CQ_MODE_MANUAL) {
        build_result = rf_cq_build_user_manual_set(
            rf_power_new_command_id(), intent_copy.desired_power_dbm,
            intent_copy.request_id, now, &rebuilt);
    } else {
        build_result = rf_cq_build_user_auto_set(
            rf_power_new_command_id(), intent_copy.desired_power_dbm,
            intent_copy.request_id, now, &rebuilt);
    }

    if (build_result != RF_CQ_OK) {
        ESP_LOGW(TAG, "RF power queue: retry build failed");
        return;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (!s_rf_user_intent_pending ||
        s_rf_user_intent_queued ||
        s_rf_report_waiting ||
        s_rf_report_submitting ||
        s_rf_dispatch_active ||
        s_rf_user_intent_generation != s_rf_policy_generation ||
        generation != s_rf_policy_generation) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    state.now_ms = now;
    state.node_addr = s_rf_node_addr;
    state.node_power_dbm = s_rf_node_power_dbm;
    state.report_waiting = s_rf_report_waiting;
    state.wait_deadline_ms =
        s_rf_wait_started_ms == 0U
            ? 0U
            : s_rf_wait_started_ms + RF_POWER_SETTLE_MS;
    state.packet_sequence = s_rf_packet_sequence;
    created = rf_dispatch_plan_create(&rebuilt, &state, &plan);

    if (!created) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    if (plan.status == RF_DISPATCH_PLAN_SEND) {
        should_submit = true;
    } else if (plan.status == RF_DISPATCH_PLAN_NO_SEND_POLICY_SAVED ||
               plan.status == RF_DISPATCH_PLAN_ERR_INACTIVE) {
        s_rf_user_intent_retry_after_ms = now + RF_POWER_EVAL_MS;
    } else if (plan.status == RF_DISPATCH_PLAN_ERR_WAITING) {
        s_rf_user_intent_retry_after_ms = state.wait_deadline_ms;
    } else {
        rf_power_clear_user_intent_locked();
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (!should_submit) {
        return;
    }

    submit_err = rf_cq_submit(&rebuilt);

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (submit_err == ESP_OK &&
        s_rf_user_intent_pending &&
        s_rf_user_intent_generation == s_rf_policy_generation) {
        s_rf_user_intent = rebuilt;
        s_rf_user_intent_queued = true;
        s_rf_user_intent_started_ms = started;
    } else if (s_rf_user_intent_generation == s_rf_policy_generation) {
        s_rf_user_intent_queued = false;
        s_rf_user_intent_retry_after_ms = now + 100U;
    }
    taskEXIT_CRITICAL(&s_rf_power_lock);
}

static bool rf_power_queued_set_auto(const char *request_id)
{
    rf_dispatch_cmd_t cmd;
    int8_t manual_power_dbm = RF_POWER_AUTO_MIN_DBM;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    uint32_t generation;

    taskENTER_CRITICAL(&s_rf_power_lock);
    manual_power_dbm = s_rf_manual_power_dbm;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (rf_cq_build_user_auto_set(rf_power_new_command_id(),
                                 manual_power_dbm, request_id, now,
                                 &cmd) != RF_CQ_OK) {
        ESP_LOGW(TAG, "RF power: reject queued AUTO SET");
        return false;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    rf_power_prepare_auto_policy_locked(request_id, now);
    generation = s_rf_policy_generation;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_policy_persist(IOT_RF_MODE_AUTO, manual_power_dbm);

    taskENTER_CRITICAL(&s_rf_power_lock);
    now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (generation != s_rf_policy_generation) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return false;
    }
    rf_power_arm_user_intent_locked(&cmd, now);
    taskEXIT_CRITICAL(&s_rf_power_lock);

    rf_power_submit_new_intent(&cmd);
    return true;
}


// Experimental local RF maintenance console. It is intentionally separate from
// MQTT so manufacturing/field tests can exercise the RF queue without changing
// the host network configuration. Physical USB access is required.
#define LOCAL_RF_CONSOLE_LINE_SIZE 64u
#define LOCAL_RF_CONSOLE_STACK     4096u

static iot_usb_serial_jtag_t s_local_rf_console_io;

static char *local_rf_console_trim(char *line)
{
    char *start = line;
    size_t len;

    while (isspace((unsigned char)*start)) {
        ++start;
    }
    if (*start == '\0') {
        return start;
    }

    len = strlen(start);
    while (len > 0U && isspace((unsigned char)start[len - 1U])) {
        start[--len] = '\0';
    }
    return start;
}

static void local_rf_console_write(const char *text)
{
    size_t len = strlen(text);

    while (len > 0U) {
        int written = iot_usb_serial_jtag_write(
            &s_local_rf_console_io, text, len, 1000U);
        if (written <= 0) {
            return;
        }
        text += written;
        len -= (size_t)written;
    }
}

static void local_rf_console_help(void)
{
    local_rf_console_write(
        "Experimental local RF console\r\n"
        "Commands:\r\n"
        "  help\r\n"
        "  rf query\r\n"
        "  rf set 0\r\n"
        "  rf set -10\r\n"
        "  rf auto\r\n");
}

static void local_rf_console_accepted(const char *operation,
                                      const char *request_id)
{
    char response[64];
    int len = snprintf(response, sizeof(response),
                       "LOCAL_RF_ACCEPTED %s request_id=%s\r\n",
                       operation, request_id);
    if (len > 0) {
        local_rf_console_write(response);
    }
}

static void local_rf_console_busy(const char *operation)
{
    char response[64];
    int len = snprintf(response, sizeof(response),
                       "LOCAL_RF_BUSY %s\r\n", operation);
    if (len > 0) {
        local_rf_console_write(response);
    }
}

static void local_rf_console_dispatch(const char *line)
{
    char request_id[RF_CQ_REQUEST_ID_SIZE] = {0};
    int target_power = 0;
    char extra = '\0';
    int parsed = -1;

    rf_power_generate_cq_request_id(request_id);

    if (strcmp(line, "help") == 0) {
        local_rf_console_help();
        return;
    }
    if (strcmp(line, "rf query") == 0) {
        if (rf_power_queued_query(request_id)) {
            local_rf_console_accepted("query", request_id);
        } else {
            local_rf_console_busy("query");
        }
        return;
    }
    if (strcmp(line, "rf auto") == 0) {
        if (rf_power_queued_set_auto(request_id)) {
            local_rf_console_accepted("auto", request_id);
        } else {
            local_rf_console_busy("auto");
        }
        return;
    }

    parsed = sscanf(line, "rf set %d %c", &target_power, &extra);
    if (parsed == 1 && target_power >= -128 && target_power <= 127) {
        int8_t power = (int8_t)target_power;
        if (rf_cq_supported_power(power)) {
            char operation[24];
            int len = snprintf(operation, sizeof(operation),
                               "set %d", (int)power);
            if (len > 0 &&
                rf_power_queued_set_manual(power, request_id)) {
                local_rf_console_accepted(operation, request_id);
            } else {
                local_rf_console_busy("set");
            }
            return;
        }
    }

    local_rf_console_write("Unknown command; type help\r\n");
}

static void local_rf_console_task(void *arg)
{
    char line[LOCAL_RF_CONSOLE_LINE_SIZE] = {0};
    size_t pos = 0U;
    uint8_t byte = 0U;

    (void)arg;
    local_rf_console_write("\r\nExperimental local RF console ready; type help\r\n> ");

    for (;;) {
        int read = iot_usb_serial_jtag_read(&s_local_rf_console_io,
                                            &byte, sizeof(byte), 100U);
        if (read != (int)sizeof(byte)) {
            continue;
        }

        if (byte == '\r' || byte == '\n') {
            char *command;

            local_rf_console_write("\r\n");
            line[pos] = '\0';
            command = local_rf_console_trim(line);
            if (*command != '\0') {
                local_rf_console_dispatch(command);
            }
            pos = 0U;
            memset(line, 0, sizeof(line));
            local_rf_console_write("> ");
        } else if (byte == 0x03U) {
            pos = 0U;
            memset(line, 0, sizeof(line));
            local_rf_console_write("\r\n> ");
        } else if (isprint(byte) || byte == '\t') {
            if (pos < sizeof(line) - 1U) {
                line[pos++] = (char)byte;
                (void)iot_usb_serial_jtag_write(&s_local_rf_console_io,
                                                &byte, sizeof(byte), 1000U);
            } else {
                pos = 0U;
                memset(line, 0, sizeof(line));
                local_rf_console_write("\r\nLine too long\r\n> ");
            }
        }
    }
}

static esp_err_t local_rf_console_start(void)
{
    const iot_usb_serial_jtag_config_t config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t err = iot_usb_serial_jtag_init(&s_local_rf_console_io, &config);
    if (err != ESP_OK) {
        return err;
    }

    err = iot_usb_serial_jtag_start(&s_local_rf_console_io);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(local_rf_console_task, "local-rf",
                    LOCAL_RF_CONSOLE_STACK, NULL, 4, NULL) != pdTRUE) {
        (void)iot_usb_serial_jtag_stop(&s_local_rf_console_io);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif

void rf_power_controller_periodic(void)
{
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    rf_power_reconcile_pending_intent();
    rf_power_background_controller_periodic();
    rf_power_publish_background_terminal_notice();
    return;
#endif
    uint16_t addr = 0;
    uint32_t last_rx = 0;
    int link_lqi = -1;
    uint8_t samples = 0;
    uint8_t high_windows = 0;
    int8_t current_power = 0;
    int8_t target_power = 0;
    uint8_t mode = IOT_RF_MODE_AUTO;
    uint8_t node_mode = IOT_RF_MODE_UNKNOWN;
    int8_t manual_power = RF_POWER_AUTO_MIN_DBM;
    bool policy_pending = false;
    bool policy_user = false;
    char policy_request_id[17] = {0};
    bool report_waiting = false;
    uint32_t wait_started_ms = 0;
    uint32_t confirmed_ms = 0;
    uint32_t snapshot_generation = 0;

    taskENTER_CRITICAL(&s_rf_power_lock);
    if (now - s_rf_last_eval_ms < RF_POWER_EVAL_MS) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }
    s_rf_last_eval_ms = now;
    if (rf_tx_backoff_active_locked(now)) {
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }
    snapshot_generation = s_rf_policy_generation;
    addr = s_rf_node_addr;
    last_rx = s_rf_last_rx_ms;
    link_lqi = s_rf_lqi_ema;
    samples = s_rf_lqi_samples;
    high_windows = s_rf_lqi_high_windows;
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
    const iot_rf_transaction_input_t transaction_input = {
        .now_ms = now,
        .last_rx_ms = last_rx,
        .wait_started_ms = wait_started_ms,
        .last_confirmed_ms = confirmed_ms,
        .report_waiting = report_waiting,
        .current_power_known = current_power != RF_POWER_UNKNOWN_DBM,
    };
    iot_rf_transaction_decision_t transaction_decision;

    if (!iot_rf_transaction_evaluate(&s_rf_transaction_config,
                                     &transaction_input, &transaction_decision)) {
        ESP_LOGW(TAG, "RF power: transaction evaluation failed");
        return;
    }

    if (transaction_decision.type == IOT_RF_TRANSACTION_DECISION_STALE) {
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_lqi_ema = -1;
        s_rf_lqi_low_windows = 0;
        s_rf_lqi_high_windows = 0;
        s_rf_node_mode = IOT_RF_MODE_UNKNOWN;
        rf_power_clear_waiting_locked();
        s_rf_policy_apply_pending = true;
        s_rf_policy_user_transaction = false;
        s_rf_policy_request_id[0] = '\0';
        taskEXIT_CRITICAL(&s_rf_power_lock);
        return;
    }

    if (transaction_decision.type == IOT_RF_TRANSACTION_DECISION_QUERY) {
        uint8_t sent_seq = 0;

        if (transaction_decision.query_reason ==
            IOT_RF_TRANSACTION_QUERY_RECONCILE) {
            ESP_LOGI(TAG, "RF power: periodic reconcile current=%d dBm", current_power);
        }
        if (rf_power_send_to_node(IOT_RF_CMD_QUERY, 0, IOT_RF_MODE_AUTO,
                                  &sent_seq, snapshot_generation) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = RF_POWER_UNKNOWN_DBM;
            s_rf_expected_report_mode = IOT_RF_MODE_UNKNOWN;
            s_rf_expected_user_transaction = false;
            s_rf_expected_request_id[0] = '\0';
            s_rf_wait_started_ms = now;
            taskEXIT_CRITICAL(&s_rf_power_lock);
        }
        return;
    }

    if (transaction_decision.type == IOT_RF_TRANSACTION_DECISION_WAIT) {
        ESP_LOGI(TAG, "RF power: waiting for report seq=%u current=%d dBm",
                 s_rf_expected_report_seq, current_power);
        return;
    }


    if (mode == IOT_RF_MODE_MANUAL) {
        if (node_mode == IOT_RF_MODE_MANUAL &&
            current_power == manual_power) {
            bool already_confirmed = false;
            char response_request_id[17] = {0};

            if (policy_pending) {
                taskENTER_CRITICAL(&s_rf_power_lock);
                bool state_still_matches =
                    s_rf_policy_apply_pending &&
                    s_rf_policy_user_transaction == policy_user &&
                    s_rf_mode == IOT_RF_MODE_MANUAL &&
                    s_rf_node_addr == addr &&
                    s_rf_node_mode == IOT_RF_MODE_MANUAL &&
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
                rf_power_publish_state(addr, manual_power, IOT_RF_MODE_MANUAL,
                                       "confirmed", response_request_id);
            }
            return;
        }
        ESP_LOGW(TAG,
                 "RF power: manual policy mismatch actual=%s/%d dBm desired=%d dBm; reconcile",
                 rf_power_mode_name(node_mode), (int)current_power,
                 (int)manual_power);
        policy_pending = true;
    } else if (mode == IOT_RF_MODE_AUTO && node_mode == IOT_RF_MODE_MANUAL) {
        ESP_LOGW(TAG,
                 "RF power: node is manual but desired is automatic; reconcile");
        policy_pending = true;
    }

    if (policy_pending) {
        uint8_t sent_seq = 0;

        if (mode == IOT_RF_MODE_MANUAL) {
            if (current_power == manual_power &&
                node_mode == IOT_RF_MODE_MANUAL && !policy_user) {
                taskENTER_CRITICAL(&s_rf_power_lock);
                s_rf_policy_apply_pending = false;
                s_rf_policy_user_transaction = false;
                s_rf_policy_request_id[0] = '\0';
                taskEXIT_CRITICAL(&s_rf_power_lock);
                return;
            }
            target_power = manual_power;
            if (rf_power_send_to_node(IOT_RF_CMD_SET, target_power,
                                      IOT_RF_MODE_MANUAL,
                                      &sent_seq, snapshot_generation) == ESP_OK) {
                taskENTER_CRITICAL(&s_rf_power_lock);
                s_rf_report_waiting = true;
                s_rf_expected_report_seq = sent_seq;
                s_rf_expected_report_power = target_power;
                s_rf_expected_report_mode = IOT_RF_MODE_MANUAL;
                s_rf_expected_user_transaction = policy_user;
                strlcpy(s_rf_expected_request_id, s_rf_policy_request_id, sizeof(s_rf_expected_request_id));
                s_rf_policy_user_transaction = false;
                s_rf_policy_request_id[0] = '\0';
                s_rf_wait_started_ms = now;
                taskEXIT_CRITICAL(&s_rf_power_lock);
            }
            return;
        }

        if (node_mode == IOT_RF_MODE_AUTO && !policy_user) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_policy_apply_pending = false;
            s_rf_policy_user_transaction = false;
            s_rf_policy_request_id[0] = '\0';
            taskEXIT_CRITICAL(&s_rf_power_lock);
            return;
        }

        target_power = current_power;
        if (rf_power_send_to_node(IOT_RF_CMD_SET, target_power,
                                  IOT_RF_MODE_AUTO,
                                  &sent_seq, snapshot_generation) == ESP_OK) {
            taskENTER_CRITICAL(&s_rf_power_lock);
            s_rf_report_waiting = true;
            s_rf_expected_report_seq = sent_seq;
            s_rf_expected_report_power = target_power;
            s_rf_expected_report_mode = IOT_RF_MODE_AUTO;
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

    const iot_rf_power_policy_input_t policy_input = {
        .samples = samples,
        .lqi_ema = link_lqi,
        .current_power_dbm = current_power,
        .good_windows = high_windows,
    };
    iot_rf_power_policy_decision_t policy_decision;

    if (!iot_rf_power_policy_decide_auto(&s_rf_power_policy_config,
                                        &policy_input, &policy_decision)) {
        ESP_LOGW(TAG, "RF power: automatic policy evaluation failed");
        return;
    }

    if (!policy_decision.current_in_bounds) {
        ESP_LOGW(TAG, "RF power: current=%d dBm outside configured policy",
                 (int)current_power);
        return;
    }

    taskENTER_CRITICAL(&s_rf_power_lock);
    s_rf_lqi_low_windows = 0;
    s_rf_lqi_high_windows = policy_decision.good_windows;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (policy_decision.action == IOT_RF_POWER_ACTION_NONE) {
        ESP_LOGI(TAG,
                 "RF power: keep %d dBm lqi_ema=%d low_windows=%u high_windows=%u",
                 current_power, link_lqi, 0, policy_decision.good_windows);
        return;
    }

    target_power = policy_decision.target_power_dbm;
    uint8_t sent_seq = 0;
    if (rf_power_send_to_node(IOT_RF_CMD_SET, target_power, IOT_RF_MODE_AUTO,
                              &sent_seq, snapshot_generation) == ESP_OK) {
        // Queue acceptance is not proof. Accept only the node's encrypted
        // REPORT carrying this request sequence, requested power and mode.
        taskENTER_CRITICAL(&s_rf_power_lock);
        s_rf_report_waiting = true;
        s_rf_expected_report_seq = sent_seq;
        s_rf_expected_report_power = target_power;
        s_rf_expected_report_mode = IOT_RF_MODE_AUTO;
        s_rf_wait_started_ms = now;
        taskEXIT_CRITICAL(&s_rf_power_lock);
    }
}



// ==================== RF health check ====================

static uint32_t rf_health_timer_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// The exact lower-layer message is emitted from the precompiled ZBOSS port.
// Hook the shared log sink so ordinary child offline/ESP_ERR_NO_MEM cases are
// not mistaken for a MAC buffer storm.
static int rf_health_log_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;

    va_copy(copy, args);
    vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (strstr(line, "ZB_ESP_MAC") != NULL &&
        strstr(line, "No buffer, handle it") != NULL) {
        uint32_t now = rf_health_timer_ms();
        uint32_t previous_last =
            __atomic_load_n(&s_rf_last_mac_nobuffer_ms, __ATOMIC_ACQUIRE);
        uint32_t events;

        // Start a new burst after a quiet gap so old transient events cannot
        // satisfy the 100-events/60-second storm criterion much later.
        if (previous_last != 0 &&
            now - previous_last > RF_HEALTH_MAC_NOBUFFER_FRESH_MS) {
            __atomic_store_n(&s_rf_first_mac_nobuffer_ms, now,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&s_rf_mac_nobuffer_events, 1,
                             __ATOMIC_RELEASE);
            events = 1;
        } else {
            events = __atomic_add_fetch(&s_rf_mac_nobuffer_events, 1,
                                       __ATOMIC_ACQ_REL);
            if (events == 1) {
                __atomic_store_n(&s_rf_first_mac_nobuffer_ms, now,
                                 __ATOMIC_RELEASE);
            }
        }
        __atomic_store_n(&s_rf_last_mac_nobuffer_ms, now, __ATOMIC_RELEASE);
    }

    return s_original_log_vprintf(fmt, args);
}

static void rf_health_log_init(void)
{
    s_original_log_vprintf = esp_log_set_vprintf(rf_health_log_vprintf);
}

void rf_power_controller_health_check(void)
{
    uint32_t now_ticks = pdTICKS_TO_MS(xTaskGetTickCount());
    uint32_t now_timer = rf_health_timer_ms();
    uint16_t addr = 0;
    uint32_t last_rx = 0;
    uint32_t mac_events = __atomic_load_n(&s_rf_mac_nobuffer_events,
                                         __ATOMIC_ACQUIRE);
    uint32_t first_mac_event = __atomic_load_n(&s_rf_first_mac_nobuffer_ms,
                                              __ATOMIC_ACQUIRE);
    uint32_t last_mac_event = __atomic_load_n(&s_rf_last_mac_nobuffer_ms,
                                             __ATOMIC_ACQUIRE);

    taskENTER_CRITICAL(&s_rf_power_lock);
    addr = s_rf_node_addr;
    last_rx = s_rf_last_rx_ms;
    taskEXIT_CRITICAL(&s_rf_power_lock);

    if (addr == 0 || last_rx == 0 ||
        now_ticks - last_rx < RF_HEALTH_NO_RX_RESTART_MS ||
        mac_events < RF_HEALTH_MAC_NOBUFFER_EVENTS ||
        first_mac_event == 0 ||
        now_timer - first_mac_event < RF_HEALTH_MAC_NOBUFFER_SPAN_MS ||
        now_timer - last_mac_event > RF_HEALTH_MAC_NOBUFFER_FRESH_MS) {
        return;
    }

    ESP_LOGW(TAG,
             "Zigbee health: active MAC no-buffer storm (%lu events over "
             "%lu ms) and no valid child frame for %lu ms; controlled restart",
             (unsigned long)mac_events,
             (unsigned long)(now_timer - first_mac_event),
             (unsigned long)(now_ticks - last_rx));
    // Give logging and the retained offline status a brief chance to flush.
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

uint16_t rf_power_controller_get_active_addr(void)
{
    uint16_t addr;
    taskENTER_CRITICAL(&s_rf_power_lock);
    addr = s_rf_node_addr;
    taskEXIT_CRITICAL(&s_rf_power_lock);
    return addr;
}


// ==================== Public helpers used by main.c event handling ====================

void rf_power_controller_publish_querying(uint16_t addr, const char *request_id)
{
    rf_power_publish_querying(addr, request_id);
}

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
esp_err_t rf_power_controller_local_console_start(void)
{
    return local_rf_console_start();
}
#endif
