#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Application callbacks for MQTT connection and application data events. */
typedef struct {
    /** Called on the MQTT event task after a CONNACK success. */
    void (*on_connected)(void *arg);

    /** Called on the MQTT event task after the transport has disconnected. */
    void (*on_disconnected)(void *arg);

    /**
     * Called on the MQTT event task for PUBLISH data. Topic and data are
     * temporary NUL-terminated copies and remain valid only during this call.
     */
    void (*on_data)(const char *topic, const char *data, void *arg);
} iot_mqtt_client_callbacks_t;

/** Last-will configuration; strings are borrowed for the lifetime of create. */
typedef struct {
    const char *topic;
    const char *message;
    int message_len;
    int qos;
    bool retain;
} iot_mqtt_client_last_will_t;

/** Static MQTT client configuration; all strings are borrowed. */
typedef struct {
    const char *uri;
    const char *username;
    const char *password;
    const char *client_id;
    iot_mqtt_client_last_will_t last_will;
} iot_mqtt_client_config_t;

/** Opaque MQTT client. */
typedef struct iot_mqtt_client iot_mqtt_client_t;

/**
 * Allocate and initialize an MQTT client without starting the transport.
 * Configuration strings must remain valid until the client is destroyed.
 */
esp_err_t iot_mqtt_client_create(
    const iot_mqtt_client_config_t *config,
    const iot_mqtt_client_callbacks_t *callbacks,
    void *callback_arg,
    iot_mqtt_client_t **client);

/** Register the event handler and start the MQTT transport. */
esp_err_t iot_mqtt_client_start(iot_mqtt_client_t *client);

/** Stop the MQTT transport without destroying the client object. */
esp_err_t iot_mqtt_client_stop(iot_mqtt_client_t *client);

/** Stop, unregister and free the MQTT client. */
esp_err_t iot_mqtt_client_destroy(iot_mqtt_client_t *client);

/** Return true only after CONNACK and before a disconnect event. */
bool iot_mqtt_client_is_connected(const iot_mqtt_client_t *client);

/** Publish a complete topic with the supplied QoS and retain flag. */
bool iot_mqtt_client_publish(iot_mqtt_client_t *client,
                             const char *topic,
                             const char *data,
                             int qos,
                             bool retain);

/** Subscribe to a complete topic filter. */
esp_err_t iot_mqtt_client_subscribe(iot_mqtt_client_t *client,
                                    const char *topic_filter,
                                    int qos);

#ifdef __cplusplus
}
#endif