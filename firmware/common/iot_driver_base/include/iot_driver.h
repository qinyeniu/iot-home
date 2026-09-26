#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*iot_driver_lifecycle_fn)(void *context);

/**
 * @brief Minimal lifecycle contract for reusable embedded drivers.
 *
 * Unused hooks may be NULL. Strongly typed driver APIs should still be
 * preferred by application code; this table is intended for uniform startup,
 * shutdown and polling when drivers are assembled into a registry.
 */
typedef struct {
    const char *name;
    iot_driver_lifecycle_fn init;
    iot_driver_lifecycle_fn start;
    iot_driver_lifecycle_fn stop;
    iot_driver_lifecycle_fn deinit;
    iot_driver_lifecycle_fn poll;
} iot_driver_t;

esp_err_t iot_driver_init(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_start(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_stop(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_deinit(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_poll(const iot_driver_t *driver, void *context);

#ifdef __cplusplus
}
#endif
