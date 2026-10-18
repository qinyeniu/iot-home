#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "iot_ssd1306.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callbacks provided by main.c so local_control can reuse existing logic. */
typedef void (*local_control_mqtt_pub_t)(const char *topic_suffix,
                                         const char *payload,
                                         bool retained);
typedef bool (*local_control_onoff_send_t)(uint16_t addr, const char *command);
typedef uint16_t (*local_control_active_child_t)(void);

typedef struct {
    local_control_mqtt_pub_t mqtt_publish;
    local_control_onoff_send_t onoff_send;
    local_control_active_child_t get_active_rf_addr;
} local_control_api_t;

/** Initialize the local control UI (rotary encoder + OLED menus). */

/** Return true when the local control UI is active and owns the OLED. */
bool local_control_is_active(void);

esp_err_t local_control_init(iot_ssd1306_t *oled,
                             const local_control_api_t *api);

#ifdef __cplusplus
}
#endif

