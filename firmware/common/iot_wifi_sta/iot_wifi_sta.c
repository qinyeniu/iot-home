#include "iot_wifi_sta.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"

static const char *TAG = "iot_wifi_sta";

static bool s_initialized;
static bool s_configured;
static bool s_started;
static bool s_owns_netif;
static bool s_owns_event_loop;
static bool s_owns_sta_netif;
static esp_netif_t *s_sta_netif;

static void iot_wifi_sta_rollback(bool deinit_wifi)
{
    if (deinit_wifi) {
        esp_wifi_deinit();
    }
    if (s_owns_sta_netif && s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
        s_owns_sta_netif = false;
    }
    if (s_owns_event_loop) {
        esp_event_loop_delete_default();
        s_owns_event_loop = false;
    }
    if (s_owns_netif) {
        esp_netif_deinit();
        s_owns_netif = false;
    }
}

esp_err_t iot_wifi_sta_initialize(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_netif_init();
    if (ret == ESP_OK) {
        s_owns_netif = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        /* Allow reuse when another startup component owns the global netif. */
        s_owns_netif = false;
    } else {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret == ESP_OK) {
        s_owns_event_loop = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        s_owns_event_loop = false;
    } else {
        iot_wifi_sta_rollback(false);
        return ret;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        iot_wifi_sta_rollback(false);
        return ESP_FAIL;
    }
    s_owns_sta_netif = true;

    const wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        iot_wifi_sta_rollback(false);
        return ret;
    }

    /* RAM storage is a component invariant. A home router may change channel,
       and stale cached BSSID/channel data must not steer scans incorrectly. */
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set RAM storage failed: %s", esp_err_to_name(ret));
        iot_wifi_sta_rollback(true);
        return ret;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t iot_wifi_sta_configure(const iot_wifi_sta_config_t *config)
{
    if (!s_initialized || s_configured || s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL || config->ssid == NULL || config->password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->power_save != WIFI_PS_NONE &&
        config->power_save != WIFI_PS_MIN_MODEM &&
        config->power_save != WIFI_PS_MAX_MODEM) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_len = strlen(config->ssid);
    const size_t password_len = strlen(config->password);
    if (ssid_len == 0U ||
        ssid_len >= sizeof(((wifi_config_t *)0)->sta.ssid) ||
        password_len < 8U ||
        password_len >= sizeof(((wifi_config_t *)0)->sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configure non-credential Wi-Fi state before placing the password in a
       stack object, reducing the scope in which it can remain resident. */
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    const wifi_country_t country = {
        .cc = {'C', 'N'},
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ret = esp_wifi_set_country(&country);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set country failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_ps(config->power_save);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set power save failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t wifi_config = {0};
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.bssid_set = false;
    wifi_config.sta.channel = 0;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.failure_retry_cnt = 3;
    wifi_config.sta.threshold.rssi = -127;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)wifi_config.sta.ssid, config->ssid,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, config->password,
            sizeof(wifi_config.sta.password));

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    memset(&wifi_config, 0, sizeof(wifi_config));
    if (ret != ESP_OK) {
        return ret;
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t iot_wifi_sta_start(void)
{
    if (!s_initialized || !s_configured || s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_wifi_start();
    if (ret == ESP_OK) {
        s_started = true;
    }
    return ret;
}

esp_err_t iot_wifi_sta_set_scan_timing(uint16_t active_max_ms,
                                       uint16_t home_chan_dwell_ms)
{
    if (!s_initialized || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (active_max_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const wifi_scan_default_params_t scan_params = {
        .scan_time.active.min = 0,
        .scan_time.active.max = active_max_ms,
        .scan_time.passive = 0,
        .home_chan_dwell_time = home_chan_dwell_ms,
    };
    return esp_wifi_set_scan_parameters(&scan_params);
}
