#include "local_control.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_rotary_encoder.h"
#include "iot_ssd1306.h"
#include "iot_rf_power_policy.h"

#define LC_TAG "local_ctrl"
#define LC_MENU_MAX_ITEMS  12
#define LC_MAX_NODES       4

/* Screen layout for SSD1306 128x64 */
#define LC_SCREEN_W  128
#define LC_SCREEN_H  64
#define LC_TITLE_Y    2
#define LC_MENU_TOP   14
#define LC_MENU_ROW_H 12
#define LC_MENU_MAX_VISIBLE ((LC_SCREEN_H - LC_MENU_TOP) / LC_MENU_ROW_H)

typedef enum {
    LC_MENU_ROOT = 0,
    LC_MENU_NODE,
    LC_MENU_RF,
    LC_MENU_INFO,
    LC_MENU_NODE_ACTIONS,
    LC_MENU_RF_ACTIONS,
} lc_menu_id_t;

typedef struct {
    const char *label;
    void (*on_enter)(void);
    void (*on_action)(int selected);
    int child_count;
    int child_base;   /* index into s_menu_items */
} lc_menu_item_t;

typedef struct {
    lc_menu_id_t id;
    const char *title;
    int item_start;
    int item_count;
} lc_menu_t;

/* Forward declarations */
static void menu_root_draw(void);
static void menu_node_enter(void);
static void menu_rf_enter(void);
static void menu_info_enter(void);
static void node_action(int selected);
static void rf_action(int selected);

/* --- State --- */
static iot_ssd1306_t *s_oled;
static local_control_api_t s_api;
static bool s_api_valid;
static QueueHandle_t s_evt_queue;
static iot_rotary_encoder_t s_rotary;
static volatile bool s_lc_active;

static lc_menu_id_t s_cur_menu = LC_MENU_ROOT;
static int s_cur_index;
static int s_node_count;
static uint16_t s_node_addrs[LC_MAX_NODES];
static const char *s_node_labels[LC_MAX_NODES];
static int s_rf_mode;          /* 0 auto, 1 manual */
static int8_t s_rf_manual_dbm;
static bool s_cmd_busy;
static char s_status_line[24];

/* --- Menu definitions --- */
static const lc_menu_item_t s_items[] = {
    /* Root menu */
    { "Node Control",  menu_node_enter, NULL, 0, 0 },
    { "RF Power",      menu_rf_enter,   NULL, 0, 0 },
    { "System Info",   menu_info_enter, NULL, 0, 0 },
    /* Node actions (dynamic labels filled at runtime) */
    { "ON",  NULL, node_action, 0, 0 },
    { "OFF", NULL, node_action, 0, 0 },
    { "TOGGLE", NULL, node_action, 0, 0 },
    /* RF actions */
    { "AUTO", NULL, rf_action, 0, 0 },
    { "MANUAL", NULL, rf_action, 0, 0 },
    { "QUERY", NULL, rf_action, 0, 0 },
};

static lc_menu_t s_menus[] = {
    { LC_MENU_ROOT,       "IOT-HOME GW", 0, 3 },
    { LC_MENU_NODE,       "Select Node", 0, 0 },
    { LC_MENU_NODE_ACTIONS, "Node Cmd",  3, 3 },
    { LC_MENU_RF,         "RF Power",    0, 0 },
    { LC_MENU_RF_ACTIONS, "RF Cmd",      6, 3 },
    { LC_MENU_INFO,       "System Info",  0, 1 },
};

/* --- Event posting helper --- */
static void lc_post(iot_rotary_event_t ev)
{
    iot_rotary_event_msg_t msg = { .event = ev, .timestamp_ms = 0 };
    if (s_evt_queue != NULL) {
        xQueueSend(s_evt_queue, &msg, 0);
    }
}

/* --- Menu drawing --- */
static void lc_draw_menu(const lc_menu_t *menu, int selected, int offset_x)
{
    iot_ssd1306_clear(s_oled);

    /* Title (yellow band rows 0-13) */
    iot_ssd1306_draw_text(s_oled, (LC_SCREEN_W - (int)strlen(menu->title) * 6) / 2 + offset_x,
                           LC_TITLE_Y, menu->title);
    iot_ssd1306_draw_hline(s_oled, 0, 13, LC_SCREEN_W, true);

    /* Items, centered around selected with scroll window */
    int first = 0;
    if (menu->item_count > LC_MENU_MAX_VISIBLE) {
        first = selected - LC_MENU_MAX_VISIBLE / 2;
        if (first < 0) first = 0;
        if (first + LC_MENU_MAX_VISIBLE > menu->item_count) {
            first = menu->item_count - LC_MENU_MAX_VISIBLE;
        }
    }

    for (int i = 0; i < menu->item_count; i++) {
        int row = i - first;
        if (row < 0 || row >= LC_MENU_MAX_VISIBLE) continue;
        int y = LC_MENU_TOP + row * LC_MENU_ROW_H;
        const lc_menu_item_t *item = &s_items[menu->item_start + i];
        const char *label = item->label;
        char tmp[24];

        if (menu->id == LC_MENU_NODE) {
            /* Node list: show short address */
            if (i < s_node_count) {
                snprintf(tmp, sizeof(tmp), "0x%04X", s_node_addrs[i]);
                label = tmp;
            }
        }

        if (i == selected) {
            iot_ssd1306_fill_rect(s_oled, 0, y, LC_SCREEN_W, LC_MENU_ROW_H - 1, true);
            iot_ssd1306_draw_text_inv(s_oled, 2 + offset_x, y + 2, label);
        } else {
            iot_ssd1306_draw_text(s_oled, 2 + offset_x, y + 2, label);
        }
    }

    /* Bottom hint */
    iot_ssd1306_draw_text(s_oled, 0, LC_SCREEN_H - 8, "ROT:sel PRESS:ok HOLD:back");
    iot_ssd1306_flush(s_oled);
}

/* --- Root draw with live status --- */
static void menu_root_draw(void)
{
    iot_ssd1306_clear(s_oled);
    iot_ssd1306_draw_text(s_oled, 28, 2, "IOT-HOME GW");
    iot_ssd1306_draw_hline(s_oled, 0, 13, LC_SCREEN_W, true);

    char line[24];
    snprintf(line, sizeof(line), "NODES: %d", s_node_count);
    iot_ssd1306_draw_text(s_oled, 2, 18, line);

    if (s_api_valid && s_api.mqtt_publish != NULL) {
        snprintf(line, sizeof(line), "MODE: LOCAL CTRL");
        iot_ssd1306_draw_text(s_oled, 2, 30, line);
    }
    snprintf(line, sizeof(line), "RF: %s", s_rf_mode == 0 ? "AUTO" : "MANUAL");
    iot_ssd1306_draw_text(s_oled, 2, 42, line);

    iot_ssd1306_draw_text(s_oled, 2, 54, "ROT:menu PRESS:open");
    iot_ssd1306_flush(s_oled);
}

/* --- Node menu --- */
static void menu_node_enter(void)
{
    s_cur_menu = LC_MENU_NODE;
    s_cur_index = 0;
    s_node_count = 0;
    /* In a real integration, main.c fills s_node_addrs via the API.
       We keep it simple: active RF child is used as the only node. */
    if (s_api_valid && s_api.get_active_rf_addr != NULL) {
        uint16_t addr = s_api.get_active_rf_addr();
        if (addr != 0xFFFF && addr != 0) {
            s_node_addrs[s_node_count++] = addr;
        }
    }
    s_menus[1].item_count = s_node_count;
    lc_draw_menu(&s_menus[LC_MENU_NODE], s_cur_index, 0);
}

static void menu_rf_enter(void)
{
    s_cur_menu = LC_MENU_RF;
    s_cur_index = 0;
    /* Auto / Manual / Query as items 0..2 */
    s_menus[3].item_count = 3;
    lc_draw_menu(&s_menus[LC_MENU_RF], s_cur_index, 0);
}

static void menu_info_enter(void)
{
    s_cur_menu = LC_MENU_INFO;
    s_cur_index = 0;
    iot_ssd1306_clear(s_oled);
    iot_ssd1306_draw_text(s_oled, 2, 2, "SYSTEM INFO");
    iot_ssd1306_draw_hline(s_oled, 0, 13, LC_SCREEN_W, true);
    iot_ssd1306_draw_text(s_oled, 2, 18, "RF SPLIT BUILD");
    iot_ssd1306_draw_text(s_oled, 2, 30, "v1.0 local");
    iot_ssd1306_draw_text(s_oled, 2, 54, "HOLD: back");
    iot_ssd1306_flush(s_oled);
}

/* --- Actions --- */
static void node_action(int selected)
{
    if (s_node_count == 0) return;
    uint16_t addr = s_node_addrs[0]; /* single node for now */
    const char *cmd = s_items[3 + selected].label;
    if (s_api_valid && s_api.onoff_send != NULL) {
        s_cmd_busy = true;
        bool ok = s_api.onoff_send(addr, cmd);
        s_cmd_busy = false;

        /* Sync to cloud */
        if (s_api_valid && s_api.mqtt_publish != NULL) {
            char topic[48];
            char payload[96];
            snprintf(topic, sizeof(topic), "nodes/gw-001/status");
            snprintf(payload, sizeof(payload),
                     "{\"event\":\"local_node_cmd\",\"node\":\"zb-%04X\",\"command\":\"%s\",\"source\":\"local\"}",
                     addr, cmd);
            s_api.mqtt_publish(topic, payload, false);
        }

        /* Feedback animation: progress bar */
        if (s_oled != NULL) {
            iot_ssd1306_clear(s_oled);
            iot_ssd1306_draw_text(s_oled, 2, 18, ok ? "CMD OK" : "CMD FAIL");
            for (int p = 0; p <= 100; p += 10) {
                iot_ssd1306_fill_rect(s_oled, 4, 40, (LC_SCREEN_W - 8) * p / 100, 6, true);
                iot_ssd1306_flush(s_oled);
                vTaskDelay(pdMS_TO_TICKS(8));
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    /* return to node menu */
    s_cur_menu = LC_MENU_NODE_ACTIONS;
    lc_draw_menu(&s_menus[LC_MENU_NODE_ACTIONS], s_cur_index, 0);
}

static void rf_action(int selected)
{
    if (s_api_valid && s_api.mqtt_publish != NULL) {
        char payload[64];
        const char *cmd = s_items[6 + selected].label;
        if (strcmp(cmd, "AUTO") == 0) {
            snprintf(payload, sizeof(payload), "{\"command\":\"rf_set_auto\",\"source\":\"local\"}");
            s_rf_mode = 0;
        } else if (strcmp(cmd, "MANUAL") == 0) {
            snprintf(payload, sizeof(payload), "{\"command\":\"rf_set_power\",\"power_dbm\":%d,\"source\":\"local\"}", (int)s_rf_manual_dbm);
            s_rf_mode = 1;
        } else {
            snprintf(payload, sizeof(payload), "{\"command\":\"rf_query_power\",\"source\":\"local\"}");
        }
        s_api.mqtt_publish("nodes/gw-001/status", payload, false);
    }

    /* progress feedback */
    if (s_oled != NULL) {
        iot_ssd1306_clear(s_oled);
        iot_ssd1306_draw_text(s_oled, 2, 18, "RF CMD SENT");
        for (int p = 0; p <= 100; p += 10) {
            iot_ssd1306_fill_rect(s_oled, 4, 40, (LC_SCREEN_W - 8) * p / 100, 6, true);
            iot_ssd1306_flush(s_oled);
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_cur_menu = LC_MENU_RF_ACTIONS;
    lc_draw_menu(&s_menus[LC_MENU_RF_ACTIONS], s_cur_index, 0);
}

/* --- Event handling --- */
static void lc_handle_event(const iot_rotary_event_msg_t *msg)
{
    const lc_menu_t *menu = &s_menus[s_cur_menu];

    switch (msg->event) {
    case IOT_ROTARY_EVENT_CW:
        if (menu->item_count > 0) {
            s_cur_index = (s_cur_index + 1) % menu->item_count;
            lc_draw_menu(menu, s_cur_index, 0);
        }
        break;
    case IOT_ROTARY_EVENT_CCW:
        if (menu->item_count > 0) {
            s_cur_index = (s_cur_index - 1 + menu->item_count) % menu->item_count;
            lc_draw_menu(menu, s_cur_index, 0);
        }
        break;
    case IOT_ROTARY_EVENT_PRESS:
        if (s_cur_menu == LC_MENU_ROOT) {
            /* Enter selected root item */
            if (s_cur_index == 0) menu_node_enter();
            else if (s_cur_index == 1) menu_rf_enter();
            else if (s_cur_index == 2) menu_info_enter();
        } else if (s_cur_menu == LC_MENU_NODE) {
            s_cur_menu = LC_MENU_NODE_ACTIONS;
            s_cur_index = 0;
            lc_draw_menu(&s_menus[LC_MENU_NODE_ACTIONS], 0, 0);
        } else if (s_cur_menu == LC_MENU_NODE_ACTIONS) {
            node_action(s_cur_index);
        } else if (s_cur_menu == LC_MENU_RF) {
            if (s_cur_index == 0) { s_rf_mode = 0; }
            else if (s_cur_index == 1) { s_rf_manual_dbm = -10; }
            s_cur_menu = LC_MENU_RF_ACTIONS;
            s_cur_index = s_cur_index < 2 ? s_cur_index : 2;
            lc_draw_menu(&s_menus[LC_MENU_RF_ACTIONS], s_cur_index, 0);
        } else if (s_cur_menu == LC_MENU_RF_ACTIONS) {
            rf_action(s_cur_index);
        }
        break;
    case IOT_ROTARY_EVENT_LONG_PRESS:
        /* Back: return to root */
        s_cur_menu = LC_MENU_ROOT;
        s_cur_index = 0;
        lc_draw_menu(&s_menus[LC_MENU_ROOT], 0, 0);
        break;
    default:
        break;
    }
}

/* --- UI task --- */
static void lc_ui_task(void *arg)
{
    (void)arg;
    iot_rotary_event_msg_t msg;
    /* Start at root menu */
    s_cur_menu = LC_MENU_ROOT;
    s_cur_index = 0;
    lc_draw_menu(&s_menus[LC_MENU_ROOT], 0, 0);

    while (s_lc_active) {
        if (xQueueReceive(s_evt_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            lc_handle_event(&msg);
        } else {
            /* Periodic refresh of root menu to keep live status */
            if (s_cur_menu == LC_MENU_ROOT) {
                menu_root_draw();
            }
        }
    }
    vTaskDelete(NULL);
}

bool local_control_is_active(void)
{
    return s_lc_active;
}

/* --- Public API --- */
esp_err_t local_control_init(iot_ssd1306_t *oled,
                             const local_control_api_t *api)
{
    if (oled == NULL || api == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_oled = oled;
    s_api = *api;
    s_api_valid = true;
    s_lc_active = true;
    s_node_count = 0;
    s_rf_mode = 0;
    s_rf_manual_dbm = -10;
    s_cmd_busy = false;

    s_evt_queue = xQueueCreate(8, sizeof(iot_rotary_event_msg_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    iot_rotary_encoder_config_t rot_cfg = {
        .pin_a = GPIO_NUM_4,
        .pin_b = GPIO_NUM_5,
        .pin_sw = GPIO_NUM_6,
        .debounce_ms = 20,
        .long_press_ms = 800,
        .event_queue = s_evt_queue,
    };
    esp_err_t ret = iot_rotary_encoder_start(&s_rotary, &rot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(LC_TAG, "rotary encoder init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_ok = xTaskCreate(lc_ui_task, "lc_ui", 4096, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        iot_rotary_encoder_stop(&s_rotary);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(LC_TAG, "local control UI started (rotary A=4 B=5 SW=6)");
    return ESP_OK;
}


