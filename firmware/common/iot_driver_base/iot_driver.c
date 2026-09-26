#include "iot_driver.h"

static esp_err_t invoke(const iot_driver_t *driver,
                        iot_driver_lifecycle_fn hook,
                        void *context)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hook == NULL) {
        return ESP_OK;
    }
    return hook(context);
}

esp_err_t iot_driver_init(const iot_driver_t *driver, void *context)
{
    return invoke(driver, driver->init, context);
}

esp_err_t iot_driver_start(const iot_driver_t *driver, void *context)
{
    return invoke(driver, driver->start, context);
}

esp_err_t iot_driver_stop(const iot_driver_t *driver, void *context)
{
    return invoke(driver, driver->stop, context);
}

esp_err_t iot_driver_deinit(const iot_driver_t *driver, void *context)
{
    return invoke(driver, driver->deinit, context);
}

esp_err_t iot_driver_poll(const iot_driver_t *driver, void *context)
{
    return invoke(driver, driver->poll, context);
}
