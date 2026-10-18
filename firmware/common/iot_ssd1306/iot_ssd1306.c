#include "iot_ssd1306.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define IOT_SSD1306_I2C_TIMEOUT_MS       1000
#define IOT_SSD1306_TX_ATTEMPTS          3
#define IOT_SSD1306_BUS_RECOVERY_PULSES  9

static const char *TAG = "iot_ssd1306";

static esp_err_t oled_i2c_force_bus_idle(iot_ssd1306_t *self)
{
    /* If a transfer was interrupted and the SSD1306 holds SDA low, bit-bang
       nine SCL pulses followed by STOP, then return the pins to I2C. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << self->config.sda_gpio) |
                        (1ULL << self->config.scl_gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_set_level(self->config.sda_gpio, 1);
    gpio_set_level(self->config.scl_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    for (int i = 0; i < IOT_SSD1306_BUS_RECOVERY_PULSES; i++) {
        gpio_set_level(self->config.scl_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(self->config.scl_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    gpio_set_level(self->config.sda_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(self->config.scl_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(self->config.sda_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    gpio_reset_pin(self->config.sda_gpio);
    gpio_reset_pin(self->config.scl_gpio);
    return ESP_OK;
}

esp_err_t iot_ssd1306_bus_init(iot_ssd1306_t *self)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = self->config.sda_gpio,
        .scl_io_num = self->config.scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = self->config.i2c_freq_hz,
    };

    esp_err_t ret = i2c_param_config(self->config.i2c_port, &i2c_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_driver_install(self->config.i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        self->bus_ready = true;
    }
    return ret;
}

static esp_err_t oled_i2c_write_once(iot_ssd1306_t *self, uint8_t control,
                                     const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(handle);
    i2c_master_write_byte(handle,
                          (uint8_t)((self->config.i2c_address << 1) |
                                    I2C_MASTER_WRITE),
                          true);
    i2c_master_write_byte(handle, control, true);
    if (data != NULL && len > 0) {
        i2c_master_write(handle, data, len, true);
    }
    i2c_master_stop(handle);

    esp_err_t ret = i2c_master_cmd_begin(self->config.i2c_port, handle,
                                         pdMS_TO_TICKS(IOT_SSD1306_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(handle);
    return ret;
}

static esp_err_t oled_i2c_write(iot_ssd1306_t *self, uint8_t control,
                                const uint8_t *data, size_t len)
{
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 0; attempt < IOT_SSD1306_TX_ATTEMPTS; attempt++) {
        ret = oled_i2c_write_once(self, control, data, len);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        self->tx_retry_count++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    self->tx_failure_count++;
    return ret;
}

esp_err_t iot_ssd1306_probe(iot_ssd1306_t *self, bool *present)
{
    if (self == NULL || present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd,
                          (uint8_t)((self->config.i2c_address << 1) |
                                    I2C_MASTER_WRITE),
                          true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(self->config.i2c_port, cmd,
                                         pdMS_TO_TICKS(IOT_SSD1306_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    *present = (ret == ESP_OK);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED address 0x%02x probe failed: %s",
                 self->config.i2c_address, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t oled_panel_start_impl(iot_ssd1306_t *self,
                                       bool clear_framebuffer)
{
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!self->bus_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    static const uint8_t init_cmds_128x64[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide / oscillator */
        0xA8, 0x3F,       /* multiplex ratio 1/64 */
        0xD3, 0x00,       /* display offset 0 */
        0x40,             /* display start line 0 */
        0xA1,             /* segment remap */
        0xC8,             /* COM scan direction */
        0xDA, 0x12,       /* COM pins: 128x64 alternative mapping */
        0x81, 0xCF,       /* contrast */
        0xD9, 0xF1,       /* pre-charge period */
        0xDB, 0x40,       /* VCOMH deselect level */
        0x8D, 0x14,       /* enable charge pump */
        0xA4,             /* output follows GDDRAM */
        0xA6,             /* normal display */
        0x20, 0x01,       /* vertical addressing mode */
        0x21, 0x00, 0x7F, /* column window 0..127 */
        0x22, 0x00, 0x07, /* page window 0..7 */
        0xAF,             /* display on */
    };
    static const uint8_t init_cmds_128x32[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide / oscillator */
        0xA8, 0x1F,       /* multiplex ratio 1/32 */
        0xD3, 0x00,       /* display offset 0 */
        0x40,             /* display start line 0 */
        0xA1,             /* segment remap */
        0xC8,             /* COM scan direction */
        0xDA, 0x02,       /* COM pins: 128x32 mapping */
        0x81, 0x8F,       /* contrast */
        0xD9, 0xF1,       /* pre-charge period */
        0xDB, 0x40,       /* VCOMH deselect level */
        0x8D, 0x14,       /* enable charge pump */
        0xA4,             /* output follows GDDRAM */
        0xA6,             /* normal display */
        0x20, 0x01,       /* vertical addressing mode */
        0x21, 0x00, 0x7F, /* column window 0..127 */
        0x22, 0x00, 0x03, /* page window 0..3 */
        0xAF,             /* display on */
    };
    const uint8_t *init_cmds;
    size_t init_cmd_len;

    if (self->config.width == IOT_SSD1306_WIDTH_128 &&
        self->config.height == IOT_SSD1306_HEIGHT_32) {
        init_cmds = init_cmds_128x32;
        init_cmd_len = sizeof(init_cmds_128x32);
    } else if (self->config.width == IOT_SSD1306_WIDTH_128 &&
               self->config.height == IOT_SSD1306_HEIGHT_64) {
        init_cmds = init_cmds_128x64;
        init_cmd_len = sizeof(init_cmds_128x64);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = oled_i2c_write(self, 0x00, init_cmds, init_cmd_len);
    if (clear_framebuffer) {
        memset(self->config.framebuffer, 0,
               (size_t)self->config.width * self->config.height / 8U);
    }
    self->panel_ready = (ret == ESP_OK);
    return ret;
}

esp_err_t iot_ssd1306_panel_start(iot_ssd1306_t *self)
{
    return oled_panel_start_impl(self, true);
}

static esp_err_t oled_panel_stop(iot_ssd1306_t *self)
{
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const uint8_t stop_cmd[] = {0xAE};
    esp_err_t ret = oled_i2c_write(self, 0x00, stop_cmd, sizeof(stop_cmd));
    if (ret == ESP_OK) {
        self->panel_ready = false;
    }
    return ret;
}

esp_err_t iot_ssd1306_recover(iot_ssd1306_t *self)
{
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    self->recovery_count++;
    ESP_LOGW(TAG, "OLED refresh failed; local recovery #%lu (retries=%lu failures=%lu)",
             (unsigned long)self->recovery_count,
             (unsigned long)self->tx_retry_count,
             (unsigned long)self->tx_failure_count);

    /* Keep Zigbee, Wi-Fi and MQTT alive; rebuild only I2C and the panel. */
    i2c_driver_delete(self->config.i2c_port);
    self->bus_ready = false;
    vTaskDelay(pdMS_TO_TICKS(80));
    esp_err_t ret = oled_i2c_force_bus_idle(self);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED I2C bus idle recovery failed: %s", esp_err_to_name(ret));
    }

    ret = iot_ssd1306_bus_init(self);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OLED I2C driver recovery failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(80));

    bool present = false;
    ret = iot_ssd1306_probe(self, &present);
    if (ret != ESP_OK || !present) {
        ESP_LOGW(TAG, "OLED did not ACK after I2C bus recovery: %s",
                 esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    ret = oled_panel_start_impl(self, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED SSD1306 reinitialization failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "OLED recovered by I2C/SSD1306 local reset");
    return ESP_OK;
}

static bool oled_flush_once(iot_ssd1306_t *self)
{
    const int pages = self->config.height / 8;
    uint8_t window_cmds[] = {
        0x20, 0x01,                     /* vertical addressing mode */
        0x21, 0x00, (uint8_t)(self->config.width - 1U),
        0x22, 0x00, (uint8_t)(pages - 1),
    };

    /* Vertical addressing consumes one complete column, page 0 through the
       final page, before advancing to the next column. Convert the page-major
       framebuffer into caller-owned scratch storage, then send one frame. */
    for (int x = 0; x < self->config.width; x++) {
        for (int page = 0; page < pages; page++) {
            self->config.scratch_buffer[x * pages + page] =
                self->config.framebuffer[page * self->config.width + x];
        }
    }

    const size_t gram_size =
        (size_t)self->config.width * self->config.height / 8U;

    /* If a frame transfer is interrupted after some GRAM bytes, the panel
       pointer moves. Every retry must therefore restart from the same window. */
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < IOT_SSD1306_TX_ATTEMPTS; attempt++) {
        ret = oled_i2c_write_once(self, 0x00, window_cmds,
                                  sizeof(window_cmds));
        if (ret == ESP_OK) {
            ret = oled_i2c_write_once(self, 0x40, self->config.scratch_buffer,
                                      gram_size);
        }
        if (ret == ESP_OK) {
            return true;
        }
        self->tx_retry_count++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    self->tx_failure_count++;
    return false;
}

esp_err_t iot_ssd1306_flush(iot_ssd1306_t *self)
{
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (oled_flush_once(self)) {
        if (self->consecutive_flush_failures > 0) {
            ESP_LOGI(TAG, "OLED refresh recovered after %lu failed frame(s)",
                     (unsigned long)self->consecutive_flush_failures);
        }
        self->consecutive_flush_failures = 0;
        return ESP_OK;
    }

    self->consecutive_flush_failures++;
    if (iot_ssd1306_recover(self) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Framebuffer is preserved, so the next task tick redraws the screen. */
    return oled_flush_once(self) ? ESP_OK : ESP_FAIL;
}

esp_err_t iot_ssd1306_clear(iot_ssd1306_t *self)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(self->config.framebuffer, 0,
           (size_t)self->config.width * self->config.height / 8U);
    return ESP_OK;
}

/* Compact 5x7 ASCII font, space (0x20) through Z (0x5A). */
static const uint8_t FONT_5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x01,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x03,0x04,0x78,0x04,0x03}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
};

esp_err_t iot_ssd1306_draw_char(iot_ssd1306_t *self, int x, int y, char ch)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch == '_') {
        ch = '-';
    }
    if (ch < ' ' || ch > 'Z') {
        return ESP_OK;
    }

    int idx = ch - ' ';
    for (int col = 0; col < 5; col++) {
        uint8_t line = FONT_5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            int px = x + col;
            int py = y + row;
            if ((line & (1 << row)) &&
                px >= 0 && px < self->config.width &&
                py >= 0 && py < self->config.height) {
                self->config.framebuffer[(py / 8) * self->config.width + px] |=
                    (uint8_t)(1u << (py % 8));
            }
        }
    }
    return ESP_OK;
}

esp_err_t iot_ssd1306_draw_text(iot_ssd1306_t *self, int x, int y,
                                const char *text)
{
    if (self == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*text != '\0') {
        esp_err_t ret = iot_ssd1306_draw_char(self, x, y, *text);
        if (ret != ESP_OK) {
            return ret;
        }
        x += 6;
        text++;
    }
    return ESP_OK;
}

esp_err_t iot_ssd1306_draw_text_inv(iot_ssd1306_t *self, int x, int y,
                                    const char *text)
{
    if (self == NULL || self->config.framebuffer == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*text != '\0') {
        char ch = *text;
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (ch == '_') {
            ch = '-';
        }
        if (ch < ' ' || ch > 'Z') {
            x += 6;
            text++;
            continue;
        }
        int idx = ch - ' ';
        for (int col = 0; col < 5; col++) {
            uint8_t line = FONT_5x7[idx][col];
            for (int row = 0; row < 7; row++) {
                int px = x + col;
                int py = y + row;
                if ((line & (1 << row)) &&
                    px >= 0 && px < self->config.width &&
                    py >= 0 && py < self->config.height) {
                    self->config.framebuffer[(py / 8) * self->config.width + px] &=
                        (uint8_t)~(1u << (py % 8));
                }
            }
        }
        x += 6;
        text++;
    }
    return ESP_OK;
}

/* ---- Graphics primitives (local control UI) ---- */

esp_err_t iot_ssd1306_draw_pixel(iot_ssd1306_t *self, int x, int y, bool on)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || x >= self->config.width || y >= self->config.height) {
        return ESP_OK; /* clip silently */
    }
    const int page = y / 8;
    const uint8_t bit = (uint8_t)(1U << (y % 8));
    uint8_t *fb = &self->config.framebuffer[page * self->config.width + x];
    if (on) {
        *fb |= bit;
    } else {
        *fb &= (uint8_t)~bit;
    }
    return ESP_OK;
}

esp_err_t iot_ssd1306_draw_hline(iot_ssd1306_t *self, int x, int y, int w, bool on)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < w; i++) {
        esp_err_t ret = iot_ssd1306_draw_pixel(self, x + i, y, on);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t iot_ssd1306_draw_vline(iot_ssd1306_t *self, int x, int y, int h, bool on)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < h; i++) {
        esp_err_t ret = iot_ssd1306_draw_pixel(self, x, y + i, on);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t iot_ssd1306_draw_rect(iot_ssd1306_t *self, int x, int y, int w, int h, bool on)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret;
    ret = iot_ssd1306_draw_hline(self, x, y, w, on);
    if (ret != ESP_OK) return ret;
    ret = iot_ssd1306_draw_hline(self, x, y + h - 1, w, on);
    if (ret != ESP_OK) return ret;
    ret = iot_ssd1306_draw_vline(self, x, y, h, on);
    if (ret != ESP_OK) return ret;
    ret = iot_ssd1306_draw_vline(self, x + w - 1, y, h, on);
    return ret;
}

esp_err_t iot_ssd1306_fill_rect(iot_ssd1306_t *self, int x, int y, int w, int h, bool on)
{
    if (self == NULL || self->config.framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int row = 0; row < h; row++) {
        esp_err_t ret = iot_ssd1306_draw_hline(self, x, y + row, w, on);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t iot_ssd1306_draw_bitmap(iot_ssd1306_t *self, int x, int y,
                                  const uint8_t *bitmap, int w, int h, bool on)
{
    if (self == NULL || self->config.framebuffer == NULL || bitmap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Row-major, MSB-first: bit 7 of byte[0] is the leftmost pixel. */
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const int byte_idx = (row * ((w + 7) / 8)) + (col / 8);
            const uint8_t bit = (uint8_t)(0x80U >> (col % 8));
            bool set = (bitmap[byte_idx] & bit) != 0;
            if (set) {
                esp_err_t ret = iot_ssd1306_draw_pixel(self, x + col, y + row, on);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
        }
    }
    return ESP_OK;
}

static bool oled_ranges_overlap(const void *a, size_t a_size,
                                const void *b, size_t b_size)
{
    const uintptr_t a_start = (uintptr_t)a;
    const uintptr_t a_end = a_start + a_size;
    const uintptr_t b_start = (uintptr_t)b;
    const uintptr_t b_end = b_start + b_size;
    return a_start < b_end && b_start < a_end;
}

esp_err_t iot_ssd1306_init(iot_ssd1306_t *self,
                           const iot_ssd1306_config_t *config)
{
    if (self == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t required_fb_size;
    if (config->width == IOT_SSD1306_WIDTH_128 &&
        config->height == IOT_SSD1306_HEIGHT_32) {
        required_fb_size = IOT_SSD1306_128X32_FB_SIZE;
    } else if (config->width == IOT_SSD1306_WIDTH_128 &&
               config->height == IOT_SSD1306_HEIGHT_64) {
        required_fb_size = IOT_SSD1306_128X64_FB_SIZE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->framebuffer == NULL ||
        config->framebuffer_size < required_fb_size ||
        config->scratch_buffer == NULL ||
        config->scratch_buffer_size < required_fb_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (oled_ranges_overlap(config->framebuffer, required_fb_size,
                            config->scratch_buffer, required_fb_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->i2c_freq_hz == 0U || config->i2c_address > 0x7FU ||
        config->sda_gpio < GPIO_NUM_0 || config->sda_gpio >= GPIO_NUM_MAX ||
        config->scl_gpio < GPIO_NUM_0 || config->scl_gpio >= GPIO_NUM_MAX ||
        config->i2c_port < I2C_NUM_0 ||
        config->i2c_port >= I2C_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *self = (iot_ssd1306_t){0};
    self->config = *config;
    return iot_ssd1306_bus_init(self);
}

static esp_err_t lifecycle_bus_init(void *context)
{
    return iot_ssd1306_bus_init((iot_ssd1306_t *)context);
}

static esp_err_t lifecycle_panel_start(void *context)
{
    return iot_ssd1306_panel_start((iot_ssd1306_t *)context);
}

static esp_err_t lifecycle_panel_stop(void *context)
{
    return oled_panel_stop((iot_ssd1306_t *)context);
}

static esp_err_t lifecycle_bus_deinit(void *context)
{
    iot_ssd1306_t *self = (iot_ssd1306_t *)context;
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_driver_delete(self->config.i2c_port);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        self->bus_ready = false;
        self->panel_ready = false;
    }
    return ret;
}

static esp_err_t lifecycle_poll(void *context)
{
    return iot_ssd1306_flush((iot_ssd1306_t *)context);
}

const iot_driver_t *iot_ssd1306_driver(void)
{
    static const iot_driver_t driver = {
        .name = "ssd1306",
        .init = lifecycle_bus_init,
        .start = lifecycle_panel_start,
        .stop = lifecycle_panel_stop,
        .deinit = lifecycle_bus_deinit,
        .poll = lifecycle_poll,
    };
    return &driver;
}
