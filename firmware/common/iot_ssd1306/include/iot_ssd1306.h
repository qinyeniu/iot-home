#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "iot_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_SSD1306_WIDTH              128
#define IOT_SSD1306_HEIGHT             64
#define IOT_SSD1306_128X64_FB_SIZE     ((IOT_SSD1306_WIDTH * IOT_SSD1306_HEIGHT) / 8)

/* All APIs are task-context only and one instance is not protected for
   concurrent calls from multiple tasks. Storage must remain caller-owned. */
typedef struct {
    i2c_port_t i2c_port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint8_t i2c_address;
    uint32_t i2c_freq_hz;
    uint8_t *framebuffer;
    size_t framebuffer_size;
} iot_ssd1306_config_t;

typedef struct {
    iot_ssd1306_config_t config;
    uint32_t tx_retry_count;
    uint32_t tx_failure_count;
    uint32_t recovery_count;
    uint32_t consecutive_flush_failures;
    bool bus_ready;
    bool panel_ready;
} iot_ssd1306_t;

/**
 * @brief Validate and save the configuration, then initialize the I2C bus.
 *
 * Storage for both the instance and framebuffer is provided by the caller.
 */
esp_err_t iot_ssd1306_init(iot_ssd1306_t *self,
                           const iot_ssd1306_config_t *config);

/** Initialize or reinitialize the I2C bus for an already configured instance. */
esp_err_t iot_ssd1306_bus_init(iot_ssd1306_t *self);

/** Probe the panel by checking its I2C address ACK. */
esp_err_t iot_ssd1306_probe(iot_ssd1306_t *self, bool *present);

/** Write the SSD1306 startup sequence and clear the framebuffer. */
esp_err_t iot_ssd1306_panel_start(iot_ssd1306_t *self);

/** Rebuild the I2C bus and reinitialize the panel without resetting the MCU. */
esp_err_t iot_ssd1306_recover(iot_ssd1306_t *self);

esp_err_t iot_ssd1306_clear(iot_ssd1306_t *self);
esp_err_t iot_ssd1306_draw_char(iot_ssd1306_t *self, int x, int y, char ch);
esp_err_t iot_ssd1306_draw_text(iot_ssd1306_t *self, int x, int y,
                                const char *text);
esp_err_t iot_ssd1306_flush(iot_ssd1306_t *self);

/** Generic lifecycle table. The instance must be configured first. */
const iot_driver_t *iot_ssd1306_driver(void);

#ifdef __cplusplus
}
#endif
