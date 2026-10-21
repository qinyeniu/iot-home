#include "iot_rotary_encoder.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/pulse_cnt.h"
#include "freertos/task.h"

#define IOT_ROTARY_TAG "iot_rotary"
#define ROTARY_COUNTS_PER_DETENT 4   /* Full quadrature cycle per EC11 detent */

typedef struct {
    iot_rotary_encoder_t *self;
    QueueHandle_t queue;
    gpio_num_t pin_sw;
    uint32_t debounce_ms;
    uint32_t long_press_ms;
    int64_t press_start_ms;
    bool pressed;
    TaskHandle_t button_task;
} iot_rotary_ctx_t;

static iot_rotary_ctx_t s_ctx;

static void post_event(iot_rotary_event_t ev)
{
    if (s_ctx.queue == NULL) {
        return;
    }
    iot_rotary_event_msg_t msg = {
        .event = ev,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    BaseType_t ok = xQueueSendFromISR(s_ctx.queue, &msg, NULL);
    (void)ok;
}

static bool IRAM_ATTR pcnt_watch_cb(pcnt_unit_handle_t unit,
                                   const pcnt_watch_event_data_t *edata,
                                   void *user_ctx)
{
    (void)unit;
    (void)user_ctx;
    if (edata->zero_cross_mode == PCNT_UNIT_ZERO_CROSS_POS_ZERO) {
        post_event(IOT_ROTARY_EVENT_CW);
    } else if (edata->zero_cross_mode == PCNT_UNIT_ZERO_CROSS_NEG_ZERO) {
        post_event(IOT_ROTARY_EVENT_CCW);
    }
    return false;
}

/* Button polling state machine.
 * The old implementation used a NEGEDGE ISR + task notification, which only
 * woke on press. Release never triggered an interrupt, so a short press was
 * only detected on the NEXT press - by then held_ms exceeded long_press_ms
 * and every press was misreported as LONG_PRESS.
 *
 * This version polls the active-low switch every 5 ms with software debounce
 * and detects both press (falling edge) and release (rising edge), so short
 * and long presses are reported correctly.
 */
/* NB: ESP-IDF default tick rate is 100 Hz (10 ms/tick), so a 5 ms poll
 * maps to vTaskDelay(0) = busy loop and starves IDLE -> task WDT reset.
 * Use 20 ms (2 ticks) to yield the CPU properly. */
#define BTN_POLL_MS         20
#define BTN_DEBOUNCE_STABLE 3   /* consecutive equal samples to confirm */

static void button_task(void *arg)
{
    (void)arg;
    int stable_level = 1;      /* active-low: idle = high */
    int stable_count = 0;
    int prev_level = 1;
    int64_t press_start_ms = 0;

    for (;;) {
        int level = gpio_get_level(s_ctx.pin_sw);

        if (level == stable_level) {
            if (stable_count < BTN_DEBOUNCE_STABLE) {
                stable_count++;
            }
        } else {
            stable_count = 0;
            stable_level = level;
            vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
            continue;
        }

        if (stable_count < BTN_DEBOUNCE_STABLE) {
            vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
            continue;
        }

        /* Stable level confirmed. Detect transitions on the stable value. */
        if (stable_level == 0 && prev_level == 1) {
            /* Press detected (active low). */
            press_start_ms = esp_timer_get_time() / 1000;
            prev_level = 0;
        } else if (stable_level == 1 && prev_level == 0) {
            /* Release detected: decide short vs long. */
            int64_t held_ms = (esp_timer_get_time() / 1000) - press_start_ms;
            post_event(held_ms >= s_ctx.long_press_ms
                           ? IOT_ROTARY_EVENT_LONG_PRESS
                           : IOT_ROTARY_EVENT_PRESS);
            prev_level = 1;
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

esp_err_t iot_rotary_encoder_start(iot_rotary_encoder_t *self,
                                   const iot_rotary_encoder_config_t *config)
{
    if (self == NULL || config == NULL || config->event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->pin_a < 0 || config->pin_b < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(self, 0, sizeof(*self));
    self->config = *config;
    self->started = false;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.self = self;
    s_ctx.queue = config->event_queue;
    s_ctx.pin_sw = config->pin_sw;
    s_ctx.debounce_ms = config->debounce_ms ? config->debounce_ms : 20;
    s_ctx.long_press_ms = config->long_press_ms ? config->long_press_ms : 800;

    /* PCNT unit: count 0..(counts_per_detent-1), watch at +4/-4 via limits. */
    pcnt_unit_config_t unit_cfg = {
        .low_limit = -ROTARY_COUNTS_PER_DETENT,
        .high_limit = ROTARY_COUNTS_PER_DETENT,
        .flags.accum_count = 0,
    };
    esp_err_t ret = pcnt_new_unit(&unit_cfg, &self->pcnt_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(IOT_ROTARY_TAG, "pcnt_new_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = config->pin_a,
        .level_gpio_num = config->pin_b,
        .flags.invert_edge_input = 0,
        .flags.invert_level_input = 0,
    };
    ret = pcnt_new_channel(self->pcnt_unit, &chan_cfg, &self->pcnt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(IOT_ROTARY_TAG, "pcnt_new_channel failed: %s", esp_err_to_name(ret));
        pcnt_del_unit(self->pcnt_unit);
        self->pcnt_unit = NULL;
        return ret;
    }

    /* Glitch filter: reject pulses < 1 us (mechanical bounce). */
    pcnt_glitch_filter_config_t glitch = { .max_glitch_ns = 1000 };
    ret = pcnt_unit_set_glitch_filter(self->pcnt_unit, &glitch);
    if (ret != ESP_OK) {
        ESP_LOGW(IOT_ROTARY_TAG, "glitch filter setup: %s", esp_err_to_name(ret));
    }

    pcnt_event_callbacks_t cbs = { .on_reach = pcnt_watch_cb };
    ret = pcnt_unit_register_event_callbacks(self->pcnt_unit, &cbs, self);
    if (ret != ESP_OK) {
        ESP_LOGE(IOT_ROTARY_TAG, "register cbs failed: %s", esp_err_to_name(ret));
        pcnt_del_channel(self->pcnt_chan);
        pcnt_del_unit(self->pcnt_unit);
        return ret;
    }

    ret = pcnt_unit_enable(self->pcnt_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(IOT_ROTARY_TAG, "pcnt enable failed: %s", esp_err_to_name(ret));
        pcnt_del_channel(self->pcnt_chan);
        pcnt_del_unit(self->pcnt_unit);
        return ret;
    }

    ret = pcnt_unit_start(self->pcnt_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(IOT_ROTARY_TAG, "pcnt start failed: %s", esp_err_to_name(ret));
        pcnt_unit_disable(self->pcnt_unit);
        pcnt_del_channel(self->pcnt_chan);
        pcnt_del_unit(self->pcnt_unit);
        return ret;
    }

    /* Button: active-low input with pull-up, ISR notifies button task. */
    if (config->pin_sw >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << config->pin_sw),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&io);
        if (ret != ESP_OK) {
            ESP_LOGE(IOT_ROTARY_TAG, "button gpio config failed: %s", esp_err_to_name(ret));
            pcnt_unit_stop(self->pcnt_unit);
            pcnt_unit_disable(self->pcnt_unit);
            pcnt_del_channel(self->pcnt_chan);
            pcnt_del_unit(self->pcnt_unit);
            return ret;
        }

        BaseType_t task_ok = xTaskCreate(button_task, "rot_btn", 2048, NULL, 10, &s_ctx.button_task);
        if (task_ok != pdPASS) {
            ESP_LOGE(IOT_ROTARY_TAG, "button task create failed");
            pcnt_unit_stop(self->pcnt_unit);
            pcnt_unit_disable(self->pcnt_unit);
            pcnt_del_channel(self->pcnt_chan);
            pcnt_del_unit(self->pcnt_unit);
            return ESP_ERR_NO_MEM;
        }
    }

    self->started = true;
    ESP_LOGI(IOT_ROTARY_TAG, "rotary encoder started (A=%d B=%d SW=%d)",
             (int)config->pin_a, (int)config->pin_b, (int)config->pin_sw);
    return ESP_OK;
}

esp_err_t iot_rotary_encoder_stop(iot_rotary_encoder_t *self)
{
    if (self == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (self->started) {
        if (self->pcnt_unit != NULL) {
            pcnt_unit_stop(self->pcnt_unit);
            pcnt_unit_disable(self->pcnt_unit);
            pcnt_del_channel(self->pcnt_chan);
            pcnt_del_unit(self->pcnt_unit);
            self->pcnt_unit = NULL;
            self->pcnt_chan = NULL;
        }
        if (s_ctx.button_task != NULL) {
            vTaskDelete(s_ctx.button_task);
            s_ctx.button_task = NULL;
        }
        self->started = false;
    }
    return ESP_OK;
}

static esp_err_t lifecycle_start(void *context)
{
    return iot_rotary_encoder_start((iot_rotary_encoder_t *)context,
                                    &((iot_rotary_encoder_t *)context)->config);
}

static esp_err_t lifecycle_stop(void *context)
{
    return iot_rotary_encoder_stop((iot_rotary_encoder_t *)context);
}

const iot_driver_t *iot_rotary_encoder_driver(void)
{
    static const iot_driver_t driver = {
        .name = "rotary_encoder",
        .init = lifecycle_start,
        .start = lifecycle_start,
        .stop = lifecycle_stop,
        .deinit = lifecycle_stop,
        .poll = NULL,
    };
    return &driver;
}


