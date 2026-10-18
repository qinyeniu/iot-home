/*
 * RF power controller component - extracted from gateway main.c.
 *
 * Handles gateway-side automatic TX power control (ATPC), RF policy
 * QUERY/SET/AUTO, tx backoff and Zigbee APS report handling.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_zigbee_core.h"

/* Cluster/endpoint constants shared with main.c Zigbee endpoint setup. */
#define RF_POWER_CLUSTER_ID     0xFC10
#define RF_ZB_ENDPOINT          10

/* MQTT publish callback (registered by main.c). */
typedef void (*rf_power_controller_mqtt_pub_t)(const char *topic_suffix,
                                               const char *payload,
                                               bool retained);

/* Active-child check callback (registered by main.c). */
typedef bool (*rf_power_controller_active_child_cb_t)(uint16_t addr);

void rf_power_controller_set_mqtt_publisher(rf_power_controller_mqtt_pub_t pub);
void rf_power_controller_set_active_child_check(rf_power_controller_active_child_cb_t cb);

void rf_power_controller_init(void);
void rf_power_controller_periodic(void);
void rf_power_controller_health_check(void);

/* Zigbee callbacks (register these with the ESP-Zigbee stack). */
void rf_power_controller_aps_data_confirm(esp_zb_apsde_data_confirm_t confirm);
bool rf_power_controller_aps_indication(esp_zb_apsde_data_ind_t ind);

/* Zigbee event hooks. */
void rf_power_controller_on_announce(uint16_t addr);
void rf_power_controller_publish_querying(uint16_t addr, const char *request_id);
void rf_power_controller_on_leave(uint16_t addr);

/* State queries used by MQTT command handling in main.c. */
uint16_t rf_power_controller_get_active_addr(void);

/* MQTT command entry points. */
void rf_power_controller_mqtt_query(const char *request_id);
void rf_power_controller_mqtt_set_manual(int8_t power_dbm, const char *request_id);
void rf_power_controller_mqtt_set_auto(const char *request_id);

#if defined(RF_COMMAND_QUEUE_ENABLED) && RF_COMMAND_QUEUE_ENABLED
esp_err_t rf_power_controller_local_console_start(void);
#endif
