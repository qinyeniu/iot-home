/*
 * IoT-Home credential field limits.
 *
 * Kept independent of ESP-IDF headers so host-side validation and tests can
 * reuse the exact product limits.
 */

#pragma once

#define IOT_WIFI_SSID_MAX_LEN              32
#define IOT_WIFI_PASSWORD_MAX_LEN          63
#define IOT_MQTT_URI_MAX_LEN              160
#define IOT_MQTT_USERNAME_MAX_LEN          64
#define IOT_MQTT_PASSWORD_MAX_LEN          64
#define IOT_MQTT_CLIENT_ID_MAX_LEN         64
