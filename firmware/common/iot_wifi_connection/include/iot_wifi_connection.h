#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Actions for which the application can gate normal Wi-Fi recovery. */
typedef enum {
    IOT_WIFI_CONNECTION_ACTION_STA_START_CONNECT,
    IOT_WIFI_CONNECTION_ACTION_DISCONNECTED_HANDOFF,
    IOT_WIFI_CONNECTION_ACTION_GOT_IP,
    IOT_WIFI_CONNECTION_ACTION_RECONNECT_LOOP,
    IOT_WIFI_CONNECTION_ACTION_MANUAL_DISCONNECT,
    IOT_WIFI_CONNECTION_ACTION_MANUAL_CONNECT,
    IOT_WIFI_CONNECTION_ACTION_STOP_BEFORE_RECOVERY,
    IOT_WIFI_CONNECTION_ACTION_START_AFTER_STOP,
} iot_wifi_connection_action_t;

/** Application-supplied callbacks and policy hooks. */
typedef struct {
    bool (*action_allowed)(iot_wifi_connection_action_t action, void *arg);
    void (*on_disconnected)(int reason, int rssi, void *arg);
    void (*on_got_ip)(uint32_t ip_address, void *arg);
    void (*on_first_connected)(void *arg);
    void (*on_retry)(uint32_t retry_count, uint32_t delay_ms, void *arg);
} iot_wifi_connection_callbacks_t;

/** Static configuration for the normal Wi-Fi connection service. */
typedef struct {
    const iot_wifi_connection_callbacks_t *callbacks;
    void *callback_arg;
    uint32_t retry_base_ms;
    uint32_t retry_max_ms;
    uint16_t task_stack_size;
    uint8_t task_priority;
} iot_wifi_connection_config_t;

/**
 * @brief Create the connection task and register Wi-Fi/IP event handlers.
 *
 * Must be called after the default event loop exists and before the station is
 * started. This service owns only normal connection recovery; provisioning can
 * block it through the application-supplied action gate.
 */
esp_err_t iot_wifi_connection_initialize(
    const iot_wifi_connection_config_t *config);

/** Return true after a normal STA IP event and false after a disconnect. */
bool iot_wifi_connection_is_up(void);

/** Wait until the normal STA connection has obtained an IP address. */
esp_err_t iot_wifi_connection_wait_connected(uint32_t timeout_ms);

/** Unregister events, delete the task and release connection-service objects. */
esp_err_t iot_wifi_connection_deinitialize(void);

#ifdef __cplusplus
}
#endif
