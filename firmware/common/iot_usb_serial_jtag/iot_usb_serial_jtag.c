#include "iot_usb_serial_jtag.h"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t iot_usb_serial_jtag_init(iot_usb_serial_jtag_t *self,
                                   const iot_usb_serial_jtag_config_t *config)
{
    if (self == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->tx_buffer_size == 0U || config->rx_buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    *self = (iot_usb_serial_jtag_t){0};
    self->config = *config;
    return ESP_OK;
}

esp_err_t iot_usb_serial_jtag_start(iot_usb_serial_jtag_t *self)
{
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (self->ready) {
        return ESP_OK;
    }
    if (self->config.tx_buffer_size == 0U || self->config.rx_buffer_size == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_serial_jtag_driver_config_t driver_config = {
        .tx_buffer_size = self->config.tx_buffer_size,
        .rx_buffer_size = self->config.rx_buffer_size,
    };

    esp_err_t ret = usb_serial_jtag_driver_install(&driver_config);
    if (ret == ESP_OK) {
        self->driver_owned = true;
        self->ready = true;
        return ESP_OK;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        /* The USB serial JTAG driver was already installed outside this
           instance. It can be shared, but stop must not uninstall it. */
        self->driver_owned = false;
        self->ready = true;
        return ESP_OK;
    }
    return ret;
}

esp_err_t iot_usb_serial_jtag_stop(iot_usb_serial_jtag_t *self)
{
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!self->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    if (self->driver_owned) {
        ret = usb_serial_jtag_driver_uninstall();
    }
    self->ready = false;
    self->driver_owned = false;
    return ret;
}

int iot_usb_serial_jtag_write(iot_usb_serial_jtag_t *self,
                              const void *data,
                              size_t size,
                              uint32_t timeout_ms)
{
    if (self == NULL || data == NULL || size == 0U) {
        return -1;
    }
    if (!self->ready) {
        return -1;
    }
    return usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(timeout_ms));
}

int iot_usb_serial_jtag_read(iot_usb_serial_jtag_t *self,
                             void *data,
                             size_t size,
                             uint32_t timeout_ms)
{
    if (self == NULL || data == NULL || size == 0U) {
        return -1;
    }
    if (!self->ready) {
        return -1;
    }
    return usb_serial_jtag_read_bytes(data, size, pdMS_TO_TICKS(timeout_ms));
}

static esp_err_t lifecycle_start(void *context)
{
    return iot_usb_serial_jtag_start((iot_usb_serial_jtag_t *)context);
}

static esp_err_t lifecycle_stop(void *context)
{
    return iot_usb_serial_jtag_stop((iot_usb_serial_jtag_t *)context);
}

const iot_driver_t *iot_usb_serial_jtag_driver(void)
{
    static const iot_driver_t driver = {
        .name = "usb_serial_jtag",
        .start = lifecycle_start,
        .stop = lifecycle_stop,
        .deinit = lifecycle_stop,
    };
    return &driver;
}
