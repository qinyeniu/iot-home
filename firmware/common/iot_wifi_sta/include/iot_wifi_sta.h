#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Static configuration for a Wi-Fi station connection.
 *
 * The SSID and password strings are only copied into the ESP-IDF Wi-Fi
 * configuration in iot_wifi_sta_configure(); they must remain valid for that
 * call and are not retained by this component.
 */
typedef struct {
    const char *ssid;
    const char *password;
    wifi_ps_type_t power_save;
} iot_wifi_sta_config_t;

/**
 * @brief Create default netif/event loop objects and initialize Wi-Fi.
 *
 * The component configures Wi-Fi storage to RAM. Event handlers remain owned
 * by the application and should be registered after this call and before
 * iot_wifi_sta_start().
 *
 * This is a global ESP-IDF Wi-Fi singleton; the API is not thread-safe.
 */
esp_err_t iot_wifi_sta_initialize(void);

/** Configure the station. Must be called after initialize, before start. */
esp_err_t iot_wifi_sta_configure(const iot_wifi_sta_config_t *config);

/** Start the configured station. */
esp_err_t iot_wifi_sta_start(void);

/** Set default station scan timing after the station has started. */
esp_err_t iot_wifi_sta_set_scan_timing(uint16_t active_max_ms,
                                       uint16_t home_chan_dwell_ms);

#ifdef __cplusplus
}
#endif
