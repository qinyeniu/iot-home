#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wifiscan";

static const char *auth_name(wifi_auth_mode_t v)
{
    switch (v) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "OTHER";
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        wifi_config_t ap_config = {
        .ap = {
            .ssid = "IOT_RF_TEST",
            .channel = 6,
            .ssid_hidden = 0,
            .max_connection = 1,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_country_t country = {0};
    if (esp_wifi_get_country(&country) == ESP_OK) {
        ESP_LOGI(TAG, "country=[%c%c] schan=%d nchan=%d policy=%d",
                 country.cc[0], country.cc[1], country.schan, country.nchan, country.policy);
    }

    for (int round = 1; round <= 8; ++round) {
        wifi_scan_config_t scan_cfg = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = 120,
            .scan_time.active.max = 300,
        };

        ESP_LOGI(TAG, "===== active scan round %d =====", round);
        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));

        uint16_t count = 0;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&count));
        ESP_LOGI(TAG, "scan result count=%u", count);

        wifi_ap_record_t *aps = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
        if (aps == NULL) {
            ESP_LOGE(TAG, "scan record alloc failed");
            esp_wifi_clear_ap_list();
            continue;
        }
        uint16_t got = count;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&got, aps));
        for (int i = 0; i < got; ++i) {
            char ssid[sizeof(aps[i].ssid) + 1] = {0};
            memcpy(ssid, aps[i].ssid, sizeof(aps[i].ssid));
            bool target = strcmp(ssid, "CU_0D84") == 0;
            ESP_LOGI(TAG, "%s%2d ssid='%s' bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d auth=%s pair=%d grp=%d",
                     target ? ">>> TARGET " : "    AP ", i + 1, ssid,
                     aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                     aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
                     aps[i].primary, aps[i].rssi, auth_name(aps[i].authmode),
                     aps[i].pairwise_cipher, aps[i].group_cipher);
        }
        free(aps);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ESP_LOGI(TAG, "AP IOT_RF_TEST remains on channel 6");
}


