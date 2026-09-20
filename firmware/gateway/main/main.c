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
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
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
#include "esp_coexist.h"
#include "esp_ieee802154.h"
#include "wifi_secrets.h"
#include "mqtt_secrets.h"

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

// Forward declarations
void mqtt_pub(const char *topic_suffix, const char *data);

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
#define ZB_JOIN_BEFORE_WIFI         1  // Commission on 802.15.4 before enabling Wi-Fi
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
    uint32_t first_dirty_ms;
    uint32_t last_report_ms;
} zb_node_t;

static zb_node_t s_zb_nodes[MAX_ZB_NODES];
static portMUX_TYPE s_zb_lock = portMUX_INITIALIZER_UNLOCKED;

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
    esp_zb_device_register(create_gateway_ep());
    esp_zb_core_action_handler_register(zb_action_handler);

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
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_cb, NULL);
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "MQTT starting: %s", MQTT_BROKER_URI);
}

void mqtt_pub_retained(const char *topic_suffix, const char *data, bool retain)
{
    if (!s_mqtt_ok) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s", MQTT_TOPIC_PREFIX, topic_suffix);
    esp_mqtt_client_publish(s_mqtt, topic, data, 0, 1, retain ? 1 : 0);
}

void mqtt_pub(const char *topic_suffix, const char *data)
{
    mqtt_pub_retained(topic_suffix, data, false);
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
        zb_publish_node_status(&snap, "device_left", false);
    } else {
        ESP_LOGI(TAG, "Zigbee: Leave for unknown/inactive IEEE "
                      "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 event->ieee[7], event->ieee[6], event->ieee[5], event->ieee[4],
                 event->ieee[3], event->ieee[2], event->ieee[1], event->ieee[0]);
    }
}

static void zb_handle_report_event(const zb_app_event_t *event)
{
    bool became_active = false;
    bool first_dirty = false;
    bool need_status = false;
    zb_node_t status_snap = {0};
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
            if (need_status) {
                zb_publish_node_status(&status_snap, "device_active", true);
            }
        }
        s_zb_report_events++;
        ESP_LOGI(TAG, "Zigbee: cached report node=0x%04x cluster=0x%04x raw=%u total=%lu",
                 event->addr, event->cluster, event->raw,
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
