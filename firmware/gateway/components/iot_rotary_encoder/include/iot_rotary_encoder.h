#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "iot_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event types published to the local-control UI queue. */
typedef enum {
    IOT_ROTARY_EVENT_NONE = 0,
    IOT_ROTARY_EVENT_CW,       /* Clockwise step */
    IOT_ROTARY_EVENT_CCW,      /* Counter-clockwise step */
    IOT_ROTARY_EVENT_PRESS,    /* Button short press */
    IOT_ROTARY_EVENT_LONG_PRESS, /* Button long press (hold ~800 ms) */
} iot_rotary_event_t;

typedef struct {
    iot_rotary_event_t event;
    uint32_t timestamp_ms;
} iot_rotary_event_msg_t;

typedef struct {
    gpio_num_t pin_a;          /* Encoder A (quadrature edge) */
    gpio_num_t pin_b;          /* Encoder B (quadrature level) */
    gpio_num_t pin_sw;         /* Encoder push button (active low) */
    uint32_t debounce_ms;      /* Button debounce, default 20 */
    uint32_t long_press_ms;    /* Long-press threshold, default 800 */
    QueueHandle_t event_queue; /* Caller-created FreeRTOS queue */
} iot_rotary_encoder_config_t;

typedef struct {
    iot_rotary_encoder_config_t config;
    pcnt_unit_handle_t pcnt_unit;
    pcnt_channel_handle_t pcnt_chan;
    bool started;
} iot_rotary_encoder_t;

/**
 * @brief Create and start the rotary encoder.
 *
 * Uses the ESP32-C6 PCNT peripheral for quadrature decoding on pin_a/pin_b
 * and a GPIO ISR with software debounce for the push button. Every detent is
 * reported as IOT_ROTARY_EVENT_CW/CCW into the caller's queue. Button press /
 * long-press events are also posted.
 *
 * Storage for `self` is caller-owned.
 */
esp_err_t iot_rotary_encoder_start(iot_rotary_encoder_t *self,
                                   const iot_rotary_encoder_config_t *config);

/** Stop the PCNT unit and disable the button ISR. */
esp_err_t iot_rotary_encoder_stop(iot_rotary_encoder_t *self);

/** Lifecycle table for registry use. */
const iot_driver_t *iot_rotary_encoder_driver(void);

#ifdef __cplusplus
}
#endif

