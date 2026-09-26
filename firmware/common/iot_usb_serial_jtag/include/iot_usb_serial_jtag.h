#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "iot_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The hardware USB Serial/JTAG peripheral is a global singleton. One instance
   is used by one task; concurrent calls from multiple tasks are not supported. */
typedef struct {
    size_t tx_buffer_size;
    size_t rx_buffer_size;
} iot_usb_serial_jtag_config_t;

typedef struct {
    iot_usb_serial_jtag_config_t config;
    bool driver_owned;
    bool ready;
} iot_usb_serial_jtag_t;

/* Validate and save the transport configuration. The driver is not installed
   until start. Caller owns the instance storage. */
esp_err_t iot_usb_serial_jtag_init(iot_usb_serial_jtag_t *self,
                                   const iot_usb_serial_jtag_config_t *config);

esp_err_t iot_usb_serial_jtag_start(iot_usb_serial_jtag_t *self);
esp_err_t iot_usb_serial_jtag_stop(iot_usb_serial_jtag_t *self);

/* Task-context blocking I/O. Returns the number of bytes, or a negative error. */
int iot_usb_serial_jtag_write(iot_usb_serial_jtag_t *self,
                              const void *data,
                              size_t size,
                              uint32_t timeout_ms);
int iot_usb_serial_jtag_read(iot_usb_serial_jtag_t *self,
                             void *data,
                             size_t size,
                             uint32_t timeout_ms);

const iot_driver_t *iot_usb_serial_jtag_driver(void);

#ifdef __cplusplus
}
#endif
