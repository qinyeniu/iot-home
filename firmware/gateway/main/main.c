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
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "iot_mqtt_client.h"
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
#include "iot_product_config.h"
#include "iot_ssd1306.h"
#include "iot_wifi_sta.h"
#include "iot_wifi_connection.h"
#include "iot_wifi_recovery_policy.h"
#include "iot_rf_packet.h"
#include "iot_rf_power_policy.h"
#include "iot_rf_transaction.h"
#include "rf_power_controller.h"
#include "iot_zb_node_model.h"
#include "iot_provisioning_wifi_owner.h"
#include "cJSON.h"
#include "zcl/esp_zigbee_zcl_on_off.h"

// ==================== Configuration ====================

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
#define OLED_I2C_FREQ_HZ        100000

#define STATUS_INTERVAL_MS      30000
#define OLED_UPDATE_MS          1000

// Forward declarations
void mqtt_pub(const char *topic_suffix, const char *data);
void mqtt_pub_retained(const char *topic_suffix, const char *data, bool retain);

// ==================== Globals ====================

static const char *TAG = "iot-gw";

static iot_mqtt_client_t *s_mqtt_client;

static bool mqtt_is_connected(void)
{
    return iot_mqtt_client_is_connected(s_mqtt_client);
}

// Product runtime configuration. The defaults come from the local lab build
// headers; if a valid NVS product configuration exists, it takes precedence.
// This path never writes NVS, so it does not add Flash wear.
static iot_wifi_config_t s_product_wifi;
static iot_mqtt_config_t s_product_mqtt;
static bool s_product_wifi_from_nvs;
static bool s_product_mqtt_from_nvs;

static void product_runtime_config_init(void)
{
    iot_wifi_config_t wifi_cfg;
    iot_mqtt_config_t mqtt_cfg;
    esp_err_t wifi_err;
    esp_err_t mqtt_err;
    esp_err_t bundle_err;

    bundle_err = iot_product_config_load_bundle(&wifi_cfg, &mqtt_cfg);
    if (bundle_err == ESP_OK) {
        s_product_wifi = wifi_cfg;
        s_product_mqtt = mqtt_cfg;
        s_product_wifi_from_nvs = true;
        s_product_mqtt_from_nvs = true;
        ESP_LOGI(TAG, "Runtime config: wifi=A/B-bundle mqtt=A/B-bundle");
        return;
    }
    if (bundle_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Product A/B config unavailable: %s; trying legacy keys",
                 esp_err_to_name(bundle_err));
    }

    strlcpy(s_product_wifi.ssid, WIFI_SSID, sizeof(s_product_wifi.ssid));
    strlcpy(s_product_wifi.password, WIFI_PASSWORD,
            sizeof(s_product_wifi.password));
    strlcpy(s_product_mqtt.uri, MQTT_BROKER_URI,
            sizeof(s_product_mqtt.uri));
    strlcpy(s_product_mqtt.username, MQTT_USERNAME,
            sizeof(s_product_mqtt.username));
    strlcpy(s_product_mqtt.password, MQTT_PASSWORD,
            sizeof(s_product_mqtt.password));
    s_product_mqtt.client_id[0] = '\0';

    wifi_err = iot_product_config_load_wifi(&wifi_cfg);
    s_product_wifi_from_nvs = wifi_err == ESP_OK;
    if (s_product_wifi_from_nvs) {
        s_product_wifi = wifi_cfg;
    } else if (wifi_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Product Wi-Fi config unavailable: %s; using build default",
                 esp_err_to_name(wifi_err));
    }

    mqtt_err = iot_product_config_load_mqtt(&mqtt_cfg);
    s_product_mqtt_from_nvs = mqtt_err == ESP_OK;
    if (s_product_mqtt_from_nvs) {
        s_product_mqtt = mqtt_cfg;
    } else if (mqtt_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Product MQTT config unavailable: %s; using build default",
                 esp_err_to_name(mqtt_err));
    }

    ESP_LOGI(TAG, "Runtime config: wifi=%s mqtt=%s",
             s_product_wifi_from_nvs ? "NVS" : "build",
             s_product_mqtt_from_nvs ? "NVS" : "build");
}

static const char *product_wifi_ssid(void)
{
    return s_product_wifi.ssid;
}

static const char *product_wifi_password(void)
{
    return s_product_wifi.password;
}

static const char *product_mqtt_uri(void)
{
    return s_product_mqtt.uri;
}

static const char *product_mqtt_username(void)
{
    return s_product_mqtt.username;
}

static const char *product_mqtt_password(void)
{
    return s_product_mqtt.password;
}

static const char *product_mqtt_client_id(void)
{
    return s_product_mqtt.client_id[0] != '\0'
         ? s_product_mqtt.client_id : NULL;
}

static const char *product_mqtt_log_uri(char *buf, size_t buf_size)
{
    const char *uri = product_mqtt_uri();
    const char *scheme_end = strstr(uri, "://");
    const char *at = strchr(uri, '@');

    if (buf_size == 0U) {
        return NULL;
    }
    if (scheme_end == NULL || at == NULL || at < scheme_end + 3) {
        strlcpy(buf, uri, buf_size);
        return buf;
    }
    snprintf(buf, buf_size, "%.*s***@%s",
             (int)(scheme_end + 3 - uri), uri, at + 1);
    return buf;
}

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

static TaskHandle_t s_zigbee_task_handle;
static TaskHandle_t s_oled_task_handle;
static TaskHandle_t s_status_task_handle;
static TaskHandle_t s_zb_forward_task_handle;

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

// ==================== OLED SSD1306 ====================
// Reusable driver implementation lives in common/iot_ssd1306. main.c owns the
// static instance and framebuffer and supplies the board-specific wiring.
static uint8_t s_oled_fb[IOT_SSD1306_128X64_FB_SIZE];
static uint8_t s_oled_gram[IOT_SSD1306_128X64_FB_SIZE];
static iot_ssd1306_t s_oled;

// ==================== Zigbee ====================

#define MAX_CHILDREN                10
#define INSTALLCODE_POLICY_ENABLE   false
#define ESP_ZB_CHANNEL_MASK         (1l << 26)  // Channel 26
#define ZB_ONLY_RF_DIAG             0  // Temporary RF/coexistence diagnostic
// A gateway which has previously seen a child uses a short RF-quiet rejoin
// window before starting Wi-Fi/MQTT. Gateways without a known child still
// bring Wi-Fi up immediately, so cloud recovery is not delayed in normal
// gateway-only operation.
#define ZB_REJOIN_QUIET_TIMEOUT_MS           120000
#define ZB_REJOIN_QUIET_SAMPLE_EXTRA_MS       30000

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

static bool zb_rejoin_quiet_window_enabled(void)
{
#if ZB_ONLY_RF_DIAG
    return false;
#else
    return s_child_seen;
#endif
}

// ==================== ZCL sensor data aggregation ====================

#define EP_GW                       10      // gateway endpoint (matches sensor nodes)
#define MAX_ZB_NODES                10
#define ZB_ONOFF_COMMAND_FRESH_MS   180000

static iot_zb_node_t s_zb_nodes[MAX_ZB_NODES];
static const iot_zb_node_store_t s_zb_store = {
    .nodes = s_zb_nodes,
    .capacity = MAX_ZB_NODES,
};
static portMUX_TYPE s_zb_lock = portMUX_INITIALIZER_UNLOCKED;

// Verify the report source independently from the short address in the frame.
// This helper uses only s_zb_lock and is intentionally called before the RF
// power controller takes its own lock to avoid a lock nesting/deadlock.
static bool rf_power_known_active_child(uint16_t addr)
{
    bool active;

    portENTER_CRITICAL(&s_zb_lock);
    active = iot_zb_node_is_active(&s_zb_store, addr);
    portEXIT_CRITICAL(&s_zb_lock);
    return active;
}

// Standard On/Off commands actuate a physical endpoint. A child can disappear
// without a Zigbee leave indication, so active alone is not enough in that
// path. The current mains node reports every 10 s and the experimental SED
// reports every 60 s; 180 s tolerates three SED periods. This interim value
// will become per-node capability configuration once multi-node provisioning
// is implemented.
static bool gateway_onoff_target_ready(uint16_t addr)
{
    bool found;
    uint32_t last_seen_ms = 0;
    uint32_t now_ms;
    uint32_t age_ms;

    portENTER_CRITICAL(&s_zb_lock);
    found = iot_zb_node_get_last_seen(&s_zb_store, addr, &last_seen_ms);
    portEXIT_CRITICAL(&s_zb_lock);

    if (!found) {
        ESP_LOGW(TAG, "CMD: node=0x%04x on/off skipped: not an active child",
                 (unsigned)addr);
        return false;
    }

    if (last_seen_ms == 0U) {
        ESP_LOGW(TAG,
                 "CMD: node=0x%04x on/off skipped: no liveness timestamp",
                 (unsigned)addr);
        return false;
    }

    now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    age_ms = now_ms - last_seen_ms;
    if (age_ms > ZB_ONOFF_COMMAND_FRESH_MS) {
        ESP_LOGW(TAG,
                 "CMD: node=0x%04x on/off skipped: stale for %lu ms",
                 (unsigned)addr,
                 (unsigned long)age_ms);
        return false;
    }

    return true;
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
        if (raw == IOT_ZB_TEMP_INVALID) {
            return ESP_OK;
        }
        event.raw = (uint16_t)raw;
        break;
    }
    case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
    case ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT: {
        uint16_t raw = *(const uint16_t *)v;
        if (raw == IOT_ZB_VALUE_INVALID) {
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
    if (zb_rejoin_quiet_window_enabled()) {
        ESP_LOGI(TAG, "Zigbee: RF-quiet rejoin window; delay 500 ms before coordinator start");
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGI(TAG, "Zigbee: Waiting 10s for WiFi to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

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
    esp_zb_aps_data_confirm_handler_register(rf_power_controller_aps_data_confirm);
    esp_zb_aps_data_indication_handler_register(rf_power_controller_aps_indication);

    ESP_ERROR_CHECK(esp_zb_start(false));

    ESP_LOGI(TAG, "Zigbee stack started with ZCL reporting");

    // The dedicated stack loop is the entry point recommended by ESP-Zigbee
    // 1.6. Polling the deprecated one-shot iteration every 100 ms adds latency
    // to coordinator timers and child/rejoin responses under Wi-Fi traffic.
    esp_zb_stack_main_loop();
    ESP_LOGE(TAG, "Zigbee stack main loop returned; restarting gateway");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
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
static bool s_wifi_ever_connected = false;

/*
 * Provisioning owner gate. I3 only installs the gate and initializes normal
 * ownership. No code can acquire provision ownership yet, so current Wi-Fi
 * recovery behavior remains enabled. The mutex is a normal FreeRTOS mutex and
 * only protects this small RAM state; it is never held while calling Wi-Fi.
 */
static SemaphoreHandle_t s_provisioning_lock;
static StaticSemaphore_t s_provisioning_lock_storage;
static iot_wifi_owner_state_t s_wifi_owner;
static bool s_wifi_owner_ready;
static bool s_wifi_owner_skip_logged;
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
static iot_recovery_quiesce_t s_recovery_quiesce;
static uint32_t s_recovery_operation_id;
#endif

static bool provisioning_lock_take(void)
{
    return s_provisioning_lock != NULL &&
           xSemaphoreTake(s_provisioning_lock, portMAX_DELAY) == pdTRUE;
}

static void provisioning_lock_give(void)
{
    if (s_provisioning_lock != NULL) {
        (void)xSemaphoreGive(s_provisioning_lock);
    }
}

static bool wifi_owner_gate_allows(const char *action)
{
    bool allowed = true;
    bool should_log_skip = false;

    if (!provisioning_lock_take()) {
        return true;
    }
    allowed = iot_wifi_owner_normal_recovery_allowed(&s_wifi_owner);
    if (allowed) {
        s_wifi_owner_skip_logged = false;
    } else {
        should_log_skip = !s_wifi_owner_skip_logged;
        s_wifi_owner_skip_logged = true;
    }
    provisioning_lock_give();
    if (should_log_skip) {
        ESP_LOGI(TAG, "Provision owner active; skip %s", action);
    }
    return allowed;
}

static bool wifi_owner_sta_start_connect_allowed(void)
{
    bool allowed = true;
    bool should_log_skip = false;

    if (!provisioning_lock_take()) {
        return true;
    }
    allowed = iot_wifi_owner_sta_start_connect_allowed(&s_wifi_owner);
    if (allowed) {
        s_wifi_owner_skip_logged = false;
    } else {
        should_log_skip = !s_wifi_owner_skip_logged;
        s_wifi_owner_skip_logged = true;
    }
    provisioning_lock_give();
    if (should_log_skip) {
        ESP_LOGI(TAG, "Provision owner active; skip STA_START auto-connect");
    }
    return allowed;
}

static void wifi_owner_runtime_init(void)
{
    iot_wifi_owner_result_t owner_result;

    if (s_wifi_owner_ready) {
        return;
    }

    s_provisioning_lock = xSemaphoreCreateMutexStatic(&s_provisioning_lock_storage);
    configASSERT(s_provisioning_lock != NULL);

    owner_result = iot_wifi_owner_init(&s_wifi_owner);
    configASSERT(owner_result == IOT_WIFI_OWNER_OK);
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    configASSERT(iot_recovery_quiesce_init(&s_recovery_quiesce) ==
                 IOT_RECOVERY_QUIESCE_OK);
#endif
    s_wifi_owner_ready = true;
}

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
static uint32_t recovery_operation_next_id(void)
{
    if (s_recovery_operation_id == UINT32_MAX) {
        return 0U;
    }
    return ++s_recovery_operation_id;
}

static bool recovery_quiesce_begin(uint32_t *operation_id)
{
    uint32_t next_id;
    iot_recovery_quiesce_result_t result;

    if (operation_id == NULL) {
        return false;
    }
    next_id = recovery_operation_next_id();
    if (next_id == 0U) {
        ESP_LOGE(TAG, "RF recovery operation IDs exhausted");
        return false;
    }

    if (!provisioning_lock_take()) {
        return false;
    }
    result = iot_recovery_quiesce_begin_operation(&s_recovery_quiesce, next_id);
    provisioning_lock_give();

    if (result == IOT_RECOVERY_QUIESCE_OK) {
        *operation_id = next_id;
        return true;
    }
    if (result == IOT_RECOVERY_QUIESCE_BUSY) {
        ESP_LOGI(TAG, "Provision quiesce active; skip RF health scan");
    } else {
        ESP_LOGW(TAG, "RF recovery operation begin failed: %d", (int)result);
    }
    return false;
}

static bool recovery_quiesce_end(uint32_t operation_id,
                                 bool request_destructive_tail,
                                 bool *destructive_tail_active)
{
    iot_recovery_quiesce_result_t result;

    if (destructive_tail_active == NULL) {
        return false;
    }
    if (!provisioning_lock_take()) {
        return false;
    }
    result = iot_recovery_quiesce_end_operation(
        &s_recovery_quiesce, operation_id, request_destructive_tail,
        destructive_tail_active);
    provisioning_lock_give();

    if (result != IOT_RECOVERY_QUIESCE_OK) {
        ESP_LOGW(TAG, "RF recovery operation end failed: %d", (int)result);
        return false;
    }
    return true;
}
#endif

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
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    uint32_t operation_id = 0U;
    bool destructive_tail_claimed = false;
    bool request_destructive_tail = false;

    if (!recovery_quiesce_begin(&operation_id)) {
        return false;
    }
#endif

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
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        (void)recovery_quiesce_end(operation_id, false, &destructive_tail_claimed);
#endif
        return false;  // Scan failure is not evidence that RF is dead.
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RF health scan count failed: %s", esp_err_to_name(err));
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        (void)recovery_quiesce_end(operation_id, false, &destructive_tail_claimed);
#endif
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
                    if (strcmp((const char *)aps[i].ssid, product_wifi_ssid()) == 0) {
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

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    // Claim the non-returning deep-sleep tail atomically while still under the
    // coordinator lock. This prevents a provisioning request from completing
    // after scan end but before the destructive recovery starts.
    request_destructive_tail = ap_count == 0 &&
        iot_wifi_recovery_empty_scan_will_reach_limit(
            s_rf_empty_scans, WIFI_RF_EMPTY_LIMIT);
    if (!recovery_quiesce_end(operation_id, request_destructive_tail,
                              &destructive_tail_claimed)) {
        return false;
    }
#endif

    const iot_wifi_recovery_scan_result_t scan_result =
        ap_count > 0 ? IOT_WIFI_RECOVERY_SCAN_OK
                     : IOT_WIFI_RECOVERY_SCAN_EMPTY;
    bool deep_sleep_due = false;
    if (!iot_wifi_recovery_apply_scan_result(
            scan_result, WIFI_RF_EMPTY_LIMIT, &s_rf_empty_scans,
            &s_rf_environment_alive, &deep_sleep_due)) {
        return false;
    }

    if (ap_count > 0) {
        ESP_LOGI(TAG, "RF health scan OK: %u AP(s), target %s",
                 ap_count, target_visible ? "VISIBLE" : "not visible");
        return true;
    }

    ESP_LOGW(TAG, "RF health scan empty %d/%d (target router off would still show neighbors)",
             s_rf_empty_scans, WIFI_RF_EMPTY_LIMIT);
    if (deep_sleep_due) {
#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
        if (destructive_tail_claimed)
#endif
        {
            wifi_rf_deep_sleep_recovery();
        }
    }
    return false;
}

static const char *wifi_connection_action_name(
    iot_wifi_connection_action_t action)
{
    switch (action) {
    case IOT_WIFI_CONNECTION_ACTION_DISCONNECTED_HANDOFF:
        return "STA_DISCONNECTED reconnect handoff";
    case IOT_WIFI_CONNECTION_ACTION_GOT_IP:
        return "STA_GOT_IP normal state update";
    case IOT_WIFI_CONNECTION_ACTION_RECONNECT_LOOP:
        return "Wi-Fi reconnect loop";
    case IOT_WIFI_CONNECTION_ACTION_MANUAL_DISCONNECT:
        return "manual Wi-Fi disconnect";
    case IOT_WIFI_CONNECTION_ACTION_MANUAL_CONNECT:
        return "manual Wi-Fi connect";
    case IOT_WIFI_CONNECTION_ACTION_STOP_BEFORE_RECOVERY:
        return "Wi-Fi stop before recovery";
    case IOT_WIFI_CONNECTION_ACTION_START_AFTER_STOP:
        return "Wi-Fi start after stop";
    case IOT_WIFI_CONNECTION_ACTION_STA_START_CONNECT:
        return "STA_START auto-connect";
    default:
        return "Wi-Fi recovery";
    }
}

static bool wifi_connection_action_allowed(
    iot_wifi_connection_action_t action, void *arg)
{
    (void)arg;
    if (action == IOT_WIFI_CONNECTION_ACTION_STA_START_CONNECT) {
        return wifi_owner_sta_start_connect_allowed();
    }
    return wifi_owner_gate_allows(wifi_connection_action_name(action));
}

static void wifi_connection_on_retry(uint32_t retry_count,
                                     uint32_t delay_ms, void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "WiFi lost, retry %lu in %lums",
             (unsigned long)retry_count, (unsigned long)delay_ms);

    // Check the whole 2.4GHz environment, not only the target SSID:
    // neighbors visible + target absent means wait for the router;
    // zero APs repeatedly means the local RF needs a deep power reset.
    if (iot_wifi_recovery_health_scan_due(retry_count) &&
        wifi_owner_gate_allows("RF health scan")) {
        wifi_rf_health_scan();
    }

    // Boot watchdog: if the radio never associated since power-on
    // (cold-boot RF glitch), perform a warm reboot like pressing RST.
    if (wifi_owner_gate_allows("Wi-Fi boot watchdog")) {
        const iot_wifi_boot_input_t boot_input = {
            .ever_connected = s_wifi_ever_connected,
            .uptime_ms = (uint64_t)esp_timer_get_time() / 1000U,
            .grace_ms = WIFI_BOOT_GRACE_MS,
            .environment_alive = s_rf_environment_alive,
            .previous_reboots = boot_restart_count_get(),
            .max_reboots = WIFI_BOOT_MAX_RESTARTS,
        };
        iot_wifi_boot_decision_t boot_decision;
        if (!iot_wifi_recovery_decide_boot(&boot_input, &boot_decision)) {
            ESP_LOGW(TAG, "WiFi boot watchdog decision failed");
        } else {
            switch (boot_decision.action) {
            case IOT_WIFI_BOOT_ACTION_NONE:
                break;
            case IOT_WIFI_BOOT_ACTION_HOLD_TARGET_MISSING:
                ESP_LOGI(TAG,
                         "Boot watchdog hold: nearby APs are visible; target SSID absent or association pending, no reboot");
                break;
            case IOT_WIFI_BOOT_ACTION_REBOOT:
                boot_restart_count_set(boot_decision.next_reboots);
                ESP_LOGW(TAG, "WiFi boot watchdog: warm reboot %d/%d",
                         boot_decision.next_reboots, WIFI_BOOT_MAX_RESTARTS);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            case IOT_WIFI_BOOT_ACTION_REBOOT_LIMIT_REACHED:
                ESP_LOGE(TAG, "WiFi still down after %d warm reboots; keep retrying",
                         boot_decision.next_reboots);
                break;
            }
        }
    }
}

static void wifi_connection_on_disconnected(int reason, int rssi, void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "WiFi disconnected: reason=%d rssi=%d", reason, rssi);
}

static void wifi_connection_on_got_ip(uint32_t ip_address, void *arg)
{
    (void)arg;
    const esp_ip4_addr_t ip = { .addr = ip_address };
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip));
    ESP_LOGI(TAG, "WiFi OK, IP: %s", s_ip);
    s_rf_empty_scans = 0;
    s_rf_environment_alive = true;
}

static void wifi_connection_on_first_connected(void *arg)
{
    (void)arg;
    s_wifi_ever_connected = true;
    boot_restart_count_set(0);   // success: allow watchdog again next cold boot
}

static void wifi_start(void)
{
    bool sta_initialized = false;
    bool connection_initialized = false;

    wifi_owner_runtime_init();

    const iot_wifi_connection_callbacks_t callbacks = {
        .action_allowed = wifi_connection_action_allowed,
        .on_disconnected = wifi_connection_on_disconnected,
        .on_got_ip = wifi_connection_on_got_ip,
        .on_first_connected = wifi_connection_on_first_connected,
        .on_retry = wifi_connection_on_retry,
    };
    const iot_wifi_connection_config_t connection_config = {
        .callbacks = &callbacks,
        .retry_base_ms = WIFI_RETRY_BASE_MS,
        .retry_max_ms = WIFI_RETRY_MAX_MS,
        .task_stack_size = 5120,
        .task_priority = 4,
    };

    esp_err_t err = iot_wifi_sta_initialize();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialize failed: %s", esp_err_to_name(err));
        return;
    }
    sta_initialized = true;

    err = iot_wifi_connection_initialize(&connection_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection initialize failed: %s",
                 esp_err_to_name(err));
        goto cleanup;
    }
    connection_initialized = true;

    const iot_wifi_sta_config_t sta_config = {
        .ssid = product_wifi_ssid(),
        .password = product_wifi_password(),
        .power_save = WIFI_PS_MIN_MODEM,
    };
    err = iot_wifi_sta_configure(&sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi configure failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = iot_wifi_sta_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // The STA must be started before changing default scan timing. A longer
    // active scan helps in crowded 2.4GHz environments and on channels 12/13.
    err = iot_wifi_sta_set_scan_timing(300, 30);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set scan parameters failed: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "WiFi starting: %s", product_wifi_ssid());
    return;

cleanup:
    if (connection_initialized) {
        const esp_err_t connection_err = iot_wifi_connection_deinitialize();
        if (connection_err != ESP_OK) {
            ESP_LOGW(TAG, "WiFi connection deinitialize failed: %s",
                     esp_err_to_name(connection_err));
        }
    }
    if (sta_initialized) {
        const esp_err_t sta_err = iot_wifi_sta_deinitialize();
        if (sta_err != ESP_OK) {
            ESP_LOGW(TAG, "WiFi STA deinitialize failed: %s",
                     esp_err_to_name(sta_err));
        }
    }
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
        bool is_rf_command =
            strcmp(command_name, "rf_query_power") == 0 ||
            strcmp(command_name, "rf_set_auto") == 0 ||
            strcmp(command_name, "rf_set_power") == 0 ||
            strcmp(command_name, "rf_manual_power") == 0;
        uint16_t selected_rf_addr = rf_power_controller_get_active_addr();
        if (is_rf_command && !rf_power_known_active_child(short_addr)) {
            ESP_LOGW(TAG, "CMD rf: node=0x%04x skipped: not an active child",
                     (unsigned)short_addr);
            cJSON_Delete(root);
            return;
        }
        if (is_rf_command && selected_rf_addr != short_addr) {
            ESP_LOGW(TAG,
                     "CMD rf: node=0x%04x skipped: selected RF node is 0x%04x",
                     (unsigned)short_addr, (unsigned)selected_rf_addr);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(command_name, "rf_query_power") == 0) {
            ESP_LOGI(TAG, "CMD rf: node=0x%04x query", (unsigned)short_addr);
            rf_power_controller_mqtt_query(request_id);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(command_name, "rf_set_auto") == 0) {
            ESP_LOGI(TAG, "CMD rf: node=0x%04x automatic", (unsigned)short_addr);
            rf_power_controller_mqtt_set_auto(request_id);
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
                rf_power_controller_mqtt_set_manual(target_power_dbm, request_id);
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
    if (!gateway_onoff_target_ready(short_addr)) {
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
    uint8_t zcl_seq = 0;

    // MQTT callbacks run outside the Zigbee main loop; acquire the mandatory
    // ZBOSS lock before issuing a standard ZCL On/Off request.
    if (!esp_zb_is_started()) {
        ESP_LOGW(TAG, "CMD fwd: node=0x%04x skipped: stack not started",
                 (unsigned)short_addr);
        cJSON_Delete(root);
        return;
    }
    if (esp_zb_lock_acquire(portMAX_DELAY)) {
        zcl_seq = esp_zb_zcl_on_off_cmd_req(&req);
        esp_zb_lock_release();
    } else {
        ESP_LOGW(TAG, "CMD fwd: node=0x%04x skipped: lock unavailable",
                 (unsigned)short_addr);
    }
    ESP_LOGI(TAG, "CMD fwd: node=0x%04x cmd=%s zcl_seq=%u",
             (unsigned)short_addr, command->valuestring, (unsigned)zcl_seq);
    cJSON_Delete(root);
}

static void mqtt_client_connected(void *arg)
{
    char sub[64];

    (void)arg;
    s_zb_status_resync = true;
    snprintf(sub, sizeof(sub), "%s/nodes/+/cmd", MQTT_TOPIC_PREFIX);

    esp_err_t err = iot_mqtt_client_subscribe(s_mqtt_client, sub, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT command subscription failed: %s",
                 esp_err_to_name(err));
    }
}

static void mqtt_client_data(const char *topic,
                             const char *data,
                             void *arg)
{
    (void)arg;
    gateway_handle_node_cmd(topic, data);
}

static void mqtt_start(void)
{
    static const char gw_lwt_payload[] =
        "{\"status\":\"offline\",\"event\":\"lwt\"}";
    static const iot_mqtt_client_callbacks_t callbacks = {
        .on_connected = mqtt_client_connected,
        .on_data = mqtt_client_data,
    };
    const iot_mqtt_client_config_t cfg = {
        .uri = product_mqtt_uri(),
        .username = product_mqtt_username(),
        .password = product_mqtt_password(),
        .client_id = product_mqtt_client_id(),
        .last_will = {
            .topic = MQTT_TOPIC_PREFIX "/" MQTT_GW_STATUS_SUFFIX,
            .message = gw_lwt_payload,
            .message_len = sizeof(gw_lwt_payload) - 1,
            .qos = 1,
            .retain = true,
        },
    };

    if (s_mqtt_outbox == NULL) {
        s_mqtt_outbox = xQueueCreateStatic(
            MQTT_OUTBOX_LEN, sizeof(mqtt_outbox_msg_t),
            (uint8_t *)s_mqtt_outbox_storage, &s_mqtt_outbox_struct);
    }

    esp_err_t err = iot_mqtt_client_create(
        &cfg, &callbacks, NULL, &s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client create failed: %s", esp_err_to_name(err));
        return;
    }

    err = iot_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client start failed: %s", esp_err_to_name(err));
        iot_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return;
    }

    char safe_uri[IOT_MQTT_URI_MAX_LEN + 1];
    ESP_LOGI(TAG, "MQTT starting: %s",
             product_mqtt_log_uri(safe_uri, sizeof(safe_uri)));
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
    return iot_mqtt_client_publish(s_mqtt_client, topic, data, 1, retain);
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
        if (mqtt_is_connected()) {
            mqtt_try_publish(topic_suffix, data, true);
        }
        return;
    }

    if (mqtt_is_connected() && uxQueueMessagesWaiting(s_mqtt_outbox) == 0) {
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
    if (!mqtt_is_connected()) {
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
            iot_ssd1306_clear(&s_oled);

            // Yellow band: 11 characters * 6px = 66px, x=(128-66)/2=31.
            iot_ssd1306_draw_text(&s_oled, 31, 4, "IOT-HOME GW");

            // Blue band: compact status lines.
            snprintf(line, sizeof(line), "IP:%s", s_ip);
            iot_ssd1306_draw_text(&s_oled, 0, 18, line);

            snprintf(line, sizeof(line), "WIFI:%s MQTT:%s",
                     iot_wifi_connection_is_up() ? "OK" : "NO",
                     mqtt_is_connected() ? "OK" : "NO");
            iot_ssd1306_draw_text(&s_oled, 0, 30, line);

            snprintf(line, sizeof(line), "UP:%02d:%02d:%02d", h, m, sec);
            iot_ssd1306_draw_text(&s_oled, 0, 42, line);

            snprintf(line, sizeof(line), "ZB NODES:%d", s_zigbee_devices);
            iot_ssd1306_draw_text(&s_oled, 0, 54, line);

            iot_ssd1306_flush(&s_oled);
        }

        vTaskDelay(pdMS_TO_TICKS(OLED_UPDATE_MS));
    }
}

static const char *resource_reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

static uint32_t resource_task_stack_free_bytes(TaskHandle_t task)
{
    if (task == NULL) {
        return 0U;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(task) *
           (uint32_t)sizeof(StackType_t);
}

static void resource_log_snapshot(void)
{
    ESP_LOGI(TAG,
             "Resource: free=%lu min=%lu block=%u reset=%s "
             "stack_free[zb=%u wifi=%u oled=%u status=%u fwd=%u]",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             resource_reset_reason_name(),
             (unsigned)resource_task_stack_free_bytes(s_zigbee_task_handle),
             (unsigned)iot_wifi_connection_task_stack_free_bytes(),
             (unsigned)resource_task_stack_free_bytes(s_oled_task_handle),
             (unsigned)resource_task_stack_free_bytes(s_status_task_handle),
             (unsigned)resource_task_stack_free_bytes(s_zb_forward_task_handle));
}

static void status_task(void *arg)
{
    (void)arg;

    while (1) {
        rf_power_controller_health_check();
        if (mqtt_is_connected()) {
            int64_t uptime_s = esp_timer_get_time() / 1000000;
            const uint32_t free_heap = esp_get_free_heap_size();
            const uint32_t min_free_heap =
                esp_get_minimum_free_heap_size();
            const uint32_t largest_block =
                heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "{\"status\":\"online\",\"event\":\"heartbeat\","
                     "\"ip\":\"%s\",\"uptime\":%lld,"
                     "\"mem\":{\"free\":%lu,\"min_free\":%lu,"
                     "\"largest_block\":%lu,\"reset\":\"%s\"},"
                     "\"stack_free\":{\"zb\":%u,\"wifi\":%u,"
                     "\"oled\":%u,\"status\":%u,\"fwd\":%u}}",
                     s_ip, uptime_s,
                     (unsigned long)free_heap,
                     (unsigned long)min_free_heap,
                     (unsigned long)largest_block,
                     resource_reset_reason_name(),
                     (unsigned)resource_task_stack_free_bytes(
                         s_zigbee_task_handle),
                     (unsigned)iot_wifi_connection_task_stack_free_bytes(),
                     (unsigned)resource_task_stack_free_bytes(
                         s_oled_task_handle),
                     (unsigned)resource_task_stack_free_bytes(
                         s_status_task_handle),
                     (unsigned)resource_task_stack_free_bytes(
                         s_zb_forward_task_handle));
            mqtt_pub_retained(MQTT_GW_STATUS_SUFFIX, msg, true);
        }
        resource_log_snapshot();
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
    esp_err_t err = nvs_open("gwzb", NVS_READWRITE, &nvh);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Zigbee: child marker NVS open failed: %s",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvh, "child", 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvh);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Zigbee: child marker persist failed: %s",
                 esp_err_to_name(err));
    }
    nvs_close(nvh);
}

static void zb_publish_gateway_status(const char *event)
{
    if (!mqtt_is_connected()) {
        return;
    }

    int64_t us = esp_timer_get_time() / 1000000;
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"status\":\"online\",\"event\":\"%s\",\"ip\":\"%s\",\"uptime\":%lld}",
             event, s_ip, us);
    mqtt_pub_retained(MQTT_GW_STATUS_SUFFIX, msg, true);
}

static void zb_publish_node_status(const iot_zb_node_t *node,
                                   const char *event, bool online)
{
    if (!mqtt_is_connected() || node == NULL || node->addr == 0) {
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
    iot_zb_node_t snap[MAX_ZB_NODES];
    uint8_t n_active;

    portENTER_CRITICAL(&s_zb_lock);
    n_active = iot_zb_node_snapshot_active(&s_zb_store, snap, MAX_ZB_NODES);
    portEXIT_CRITICAL(&s_zb_lock);

    zb_publish_gateway_status("status_resync");
    for (uint8_t i = 0; i < n_active; i++) {
        zb_publish_node_status(&snap[i], "status_resync", true);
    }
}

static void zb_handle_join_event(const zb_app_event_t *event)
{
    iot_zb_node_result_t model_result = {0};
    iot_zb_node_t status_snap = {0};
    bool applied;
    bool is_new_child = false;
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

    portENTER_CRITICAL(&s_zb_lock);
    applied = iot_zb_node_apply_join(&s_zb_store, event->addr, event->ieee,
                                    now_ms, &model_result);
    if (applied && model_result.found) {
        is_new_child = model_result.became_active;
        if (is_new_child) {
            s_zigbee_devices++;
        }
        status_snap = s_zb_store.nodes[model_result.index];
    }
    portEXIT_CRITICAL(&s_zb_lock);

    if (!applied || !model_result.found) {
        ESP_LOGW(TAG, "Zigbee: join event ignored, node table full addr=0x%04x",
                 event->addr);
        return;
    }

    if (is_new_child) {
        ESP_LOGI(TAG, "Zigbee: Device joined! addr=0x%04x", event->addr);
        zb_persist_child_seen();
    } else {
        ESP_LOGI(TAG, "Zigbee: Device announced again! addr=0x%04x", event->addr);
    }

    zb_publish_node_status(&status_snap,
                           is_new_child ? "device_joined" : "device_announce",
                           true);
    rf_power_controller_on_announce(event->addr);
    rf_power_controller_publish_querying(event->addr, NULL);
}

static void zb_handle_leave_event(const zb_app_event_t *event)
{
    iot_zb_node_result_t model_result = {0};
    iot_zb_node_t snap;
    bool processed;
    bool was_active = false;

    memset(&snap, 0, sizeof(snap));
    portENTER_CRITICAL(&s_zb_lock);
    processed = iot_zb_node_apply_leave(&s_zb_store, event->ieee, &model_result);
    if (processed && model_result.was_active) {
        was_active = true;
        s_zigbee_devices--;
        snap = s_zb_store.nodes[model_result.index];
    }
    portEXIT_CRITICAL(&s_zb_lock);

    if (was_active) {
        ESP_LOGI(TAG, "Zigbee: Device left! addr=0x%04x", snap.addr);
        rf_power_controller_on_leave(snap.addr);
        zb_publish_node_status(&snap, "device_left", false);
    } else {
        ESP_LOGI(TAG, "Zigbee: Leave for unknown/inactive IEEE "
                      "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 event->ieee[7], event->ieee[6], event->ieee[5], event->ieee[4],
                 event->ieee[3], event->ieee[2], event->ieee[1], event->ieee[0]);
    }
}

static void zb_publish_node_switch_state(const iot_zb_node_t *node)
{
    char topic_suffix[40];
    char payload[64];

    if (!mqtt_is_connected() || node == NULL || !node->onoff_valid) {
        return;
    }

    snprintf(topic_suffix, sizeof(topic_suffix), "nodes/zb-%04x/telemetry", node->addr);
    snprintf(payload, sizeof(payload), "{\"data\":{\"on_off\":%u}}", node->onoff_value);
    mqtt_pub(topic_suffix, payload);
    ESP_LOGI(TAG, "MQTT -> %s: %s", topic_suffix, payload);
}

static void zb_handle_report_event(const zb_app_event_t *event)
{
    iot_zb_node_result_t model_result = {0};
    bool became_active = false;
    bool need_status = false;
    bool switch_state_changed = false;
    bool accepted = false;
    iot_zb_node_t status_snap = {0};
    iot_zb_node_t switch_snap = {0};
    uint8_t field_mask = 0;
    bool onoff_event = false;
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

    if (event->cluster == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) {
        field_mask = IOT_ZB_MASK_TEMP;
    } else if (event->cluster == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) {
        field_mask = IOT_ZB_MASK_HUM;
    } else if (event->cluster == ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT) {
        field_mask = IOT_ZB_MASK_LUX;
    } else if (event->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        onoff_event = true;
    } else {
        return;
    }

    portENTER_CRITICAL(&s_zb_lock);
    // A successful report proves the child is present even if an announce was dropped.
    if (onoff_event) {
        accepted = iot_zb_node_apply_onoff(&s_zb_store, event->addr, event->value,
                                           now_ms, &model_result);
    } else {
        accepted = iot_zb_node_apply_measurement(
            &s_zb_store, event->addr, field_mask, event->raw, now_ms,
            &model_result);
    }

    if (accepted && model_result.found) {
        became_active = model_result.became_active;
        if (became_active) {
            s_zigbee_devices++;
            status_snap = s_zb_store.nodes[model_result.index];
            need_status = true;
        }
        if (onoff_event) {
            switch_snap = s_zb_store.nodes[model_result.index];
            switch_state_changed = true;
        }
    }
    portEXIT_CRITICAL(&s_zb_lock);

    if (accepted) {
        if (became_active) {
            ESP_LOGI(TAG, "Zigbee: Device active by report addr=0x%04x", event->addr);
            zb_persist_child_seen();
            rf_power_controller_on_announce(event->addr);
            rf_power_controller_publish_querying(event->addr, NULL);
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

#define ZB_REPORT_PARTIAL_DELAY_MS  30000
#define ZB_TELEMETRY_DEDUP_MS       9000

static uint32_t s_zb_last_telemetry_ms[MAX_ZB_NODES];
static char s_zb_last_telemetry_payload[MAX_ZB_NODES][128];

static void zb_forward_pending(void)
{
    iot_zb_dirty_node_t due[MAX_ZB_NODES];
    uint8_t n_dirty;
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

    if (!mqtt_is_connected()) {
        return;
    }

    // Wait for a complete temperature/humidity/light sample to avoid duplicate
    // MQTT messages; sleepy-end-device reports can arrive more than 10s apart.
    // A longer partial timeout still publishes incomplete/legacy sensors.
    portENTER_CRITICAL(&s_zb_lock);
    n_dirty = iot_zb_node_take_due(&s_zb_store, now_ms,
                                   ZB_REPORT_PARTIAL_DELAY_MS, due,
                                   MAX_ZB_NODES);
    portEXIT_CRITICAL(&s_zb_lock);

    for (uint8_t msg = 0; msg < n_dirty; msg++) {
        const iot_zb_node_t *n = &due[msg].node;
        uint8_t slot = due[msg].index;
        char topic_suffix[40];
        char payload[160];

        snprintf(topic_suffix, sizeof(topic_suffix),
                 "nodes/zb-%04x/telemetry", n->addr);
        if (!iot_zb_node_build_telemetry(n, payload, sizeof(payload))) {
            continue;
        }

        uint32_t telemetry_age_ms = now_ms - s_zb_last_telemetry_ms[slot];
        bool duplicate_telemetry =
            s_zb_last_telemetry_payload[slot][0] != '\0' &&
            telemetry_age_ms < ZB_TELEMETRY_DEDUP_MS &&
            strcmp(payload, s_zb_last_telemetry_payload[slot]) == 0;

        if (duplicate_telemetry) {
            ESP_LOGI(TAG, "MQTT duplicate suppressed (%s, age=%lu ms): %s",
                     topic_suffix, (unsigned long)telemetry_age_ms, payload);
        } else {
            mqtt_pub(topic_suffix, payload);
            ESP_LOGI(TAG, "MQTT -> %s: %s", topic_suffix, payload);
            strlcpy(s_zb_last_telemetry_payload[slot], payload,
                    sizeof(s_zb_last_telemetry_payload[slot]));
            s_zb_last_telemetry_ms[slot] = now_ms;
        }
    }
}

#if !ZB_ONLY_RF_DIAG
static bool zb_have_full_active_sample(void)
{
    iot_zb_node_t active[MAX_ZB_NODES];
    uint8_t n_active;
    bool ready = false;

    portENTER_CRITICAL(&s_zb_lock);
    n_active = iot_zb_node_snapshot_active(&s_zb_store, active, MAX_ZB_NODES);
    for (uint8_t i = 0; i < n_active; ++i) {
        if ((active[i].valid & IOT_ZB_MASK_ALL) == IOT_ZB_MASK_ALL) {
            ready = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_zb_lock);
    return ready;
}
#endif
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

        if (s_zb_status_resync && mqtt_is_connected()) {
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

    rf_power_controller_init();
    rf_power_controller_set_mqtt_publisher(mqtt_pub_retained);
    rf_power_controller_set_active_child_check(rf_power_known_active_child);

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
    ESP_LOGI(TAG, "Wi-Fi/IEEE 802.15.4 coexistence enabled (15.4 priority LOW/MIDDLE; Wi-Fi MIN_MODEM)");
#else
    ESP_LOGW(TAG, "ZB-only RF diagnostic: Wi-Fi/coexistence disabled");
#endif
    
    // I2C / OLED bus
    const iot_ssd1306_config_t oled_config = {
        .width = OLED_WIDTH,
        .height = OLED_HEIGHT,
        .i2c_port = I2C_NUM_0,
        .sda_gpio = I2C_SDA_PIN,
        .scl_gpio = I2C_SCL_PIN,
        .i2c_address = OLED_ADDR,
        .i2c_freq_hz = OLED_I2C_FREQ_HZ,
        .framebuffer = s_oled_fb,
        .framebuffer_size = sizeof(s_oled_fb),
        .scratch_buffer = s_oled_gram,
        .scratch_buffer_size = sizeof(s_oled_gram),
    };
    ret = iot_ssd1306_init(&s_oled, &oled_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C init failed (SDA=%d SCL=%d): %s",
                 I2C_SDA_PIN, I2C_SCL_PIN, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "I2C init OK (SDA=%d SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);
    }

    // OLED: reusable SSD1306 text driver
    bool oled_present = false;
    esp_err_t oled_probe_ret = ESP_FAIL;
    esp_err_t oled_panel_ret = ESP_FAIL;
    if (ret == ESP_OK) {
        oled_probe_ret = iot_ssd1306_probe(&s_oled, &oled_present);
    }
    if (ret == ESP_OK && oled_probe_ret == ESP_OK && oled_present) {
        oled_panel_ret = iot_ssd1306_panel_start(&s_oled);
    }
    if (ret == ESP_OK && oled_probe_ret == ESP_OK && oled_present &&
        oled_panel_ret == ESP_OK) {
        s_oled_available = true;
        iot_ssd1306_clear(&s_oled);
        iot_ssd1306_draw_text(&s_oled, 31, 4, "IOT-HOME GW");
        iot_ssd1306_draw_text(&s_oled, 31, 30, "STARTING...");
        if (iot_ssd1306_flush(&s_oled) == ESP_OK) {
            ESP_LOGI(TAG, "OLED init OK (reusable SSD1306 text driver)");
        } else {
            ESP_LOGW(TAG, "OLED initialized but first flush failed");
        }
    } else {
        ESP_LOGW(TAG, "OLED not available (init=%s probe=%s present=%d panel=%s), running without display",
                 esp_err_to_name(ret), esp_err_to_name(oled_probe_ret),
                 oled_present ? 1 : 0, esp_err_to_name(oled_panel_ret));
    }

    
#if !ZB_ONLY_RF_DIAG
    product_runtime_config_init();
#endif

#if !ZB_ONLY_RF_DIAG
    if (zb_rejoin_quiet_window_enabled()) {
        ESP_LOGI(TAG, "Defer Wi-Fi/MQTT until Zigbee RF-quiet rejoin completes");
    } else {
        // WiFi
        wifi_start();
        (void)iot_wifi_connection_wait_connected(30000U);

        // MQTT
        mqtt_start();
    }
#else
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
    BaseType_t zigbee_task_ok =
        xTaskCreate(zigbee_task, "zigbee", 16384, NULL, 5,
                    &s_zigbee_task_handle);
    if (zigbee_task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    BaseType_t oled_task_ok =
        xTaskCreate(oled_task, "oled", 4096, NULL, 5,
                    &s_oled_task_handle);
    BaseType_t status_task_ok =
        xTaskCreate(status_task, "status", 6144, NULL, 5,
                    &s_status_task_handle);
    BaseType_t forward_task_ok =
        xTaskCreate(zb_forward_task, "zb-fwd", 5120, NULL, 5,
                    &s_zb_forward_task_handle);
    if (oled_task_ok != pdPASS || status_task_ok != pdPASS ||
        forward_task_ok != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create required task: oled=%d status=%d fwd=%d",
                 (int)oled_task_ok, (int)status_task_ok,
                 (int)forward_task_ok);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

#if !ZB_ONLY_RF_DIAG
    if (zb_rejoin_quiet_window_enabled()) {
        ESP_LOGI(TAG,
                 "Zigbee rejoin RF window: %d ms; up to %d ms before Wi-Fi start",
                 ZB_REJOIN_QUIET_TIMEOUT_MS,
                 ZB_REJOIN_QUIET_TIMEOUT_MS + ZB_REJOIN_QUIET_SAMPLE_EXTRA_MS);

        int join_wait_ms = 0;
        while (join_wait_ms < ZB_REJOIN_QUIET_TIMEOUT_MS &&
               s_zigbee_devices == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            join_wait_ms += 500;
        }

        bool join_seen = (s_zigbee_devices > 0);
        bool first_sample_ready = false;
        if (join_seen) {
            int sample_wait_limit_ms =
                join_wait_ms + ZB_REJOIN_QUIET_SAMPLE_EXTRA_MS;
            ESP_LOGI(TAG, "Zigbee join announced at %d ms; wait up to %d ms for a full sensor sample",
                     join_wait_ms, sample_wait_limit_ms);
            while (join_wait_ms < sample_wait_limit_ms) {
                if (zb_have_full_active_sample()) {
                    first_sample_ready = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                join_wait_ms += 500;
            }
        }

        if (!join_seen) {
            ESP_LOGW(TAG, "No Zigbee join in %d ms; start Wi-Fi anyway",
                     ZB_REJOIN_QUIET_TIMEOUT_MS);
        } else if (first_sample_ready) {
            ESP_LOGI(TAG, "Full Zigbee sample ready at %d ms; start Wi-Fi/MQTT",
                     join_wait_ms);
        } else {
            ESP_LOGW(TAG, "Join seen but no full sample by %d ms; start Wi-Fi anyway",
                     join_wait_ms);
        }

        wifi_start();
        (void)iot_wifi_connection_wait_connected(30000U);
        mqtt_start();
    }
#endif
    
    ESP_LOGI(TAG, "Gateway ready!");

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
    ret = rf_power_controller_local_console_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Local RF console unavailable: %s", esp_err_to_name(ret));
    }
#endif
    
    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        if (mqtt_is_connected()) {
            char msg[64];
            snprintf(msg, sizeof(msg), "{\"gw\":\"online\"}");
            mqtt_pub("nodes/gw-001/telemetry", msg);
        }
    }
}

