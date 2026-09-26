/*
 * Provisioning worker lifecycle implementation.
 */

#include "iot_provisioning_worker.h"

#include <string.h>

iot_prov_worker_result_t iot_prov_worker_init(iot_prov_worker_t *worker)
{
    if (worker == NULL) {
        return IOT_PROV_WORKER_ERR_INVALID_ARG;
    }

    memset(worker, 0, sizeof(*worker));
    worker->initialized = true;
    worker->state = IOT_PROV_WORKER_STOPPED;
    return IOT_PROV_WORKER_OK;
}

iot_prov_worker_result_t iot_prov_worker_request_start(
    iot_prov_worker_t *worker)
{
    if (worker == NULL || !worker->initialized) {
        return IOT_PROV_WORKER_ERR_INVALID_ARG;
    }
    if (worker->state != IOT_PROV_WORKER_STOPPED) {
        return IOT_PROV_WORKER_ERR_INVALID_STATE;
    }

    ++worker->generation;
    if (worker->generation == 0U) {
        worker->generation = 1U;
    }
    worker->state = IOT_PROV_WORKER_STARTING;
    return IOT_PROV_WORKER_OK;
}

iot_prov_worker_result_t iot_prov_worker_note_running(
    iot_prov_worker_t *worker)
{
    if (worker == NULL || !worker->initialized) {
        return IOT_PROV_WORKER_ERR_INVALID_ARG;
    }
    if (worker->state == IOT_PROV_WORKER_STARTING) {
        worker->state = IOT_PROV_WORKER_RUNNING;
    } else if (worker->state != IOT_PROV_WORKER_STOPPING) {
        return IOT_PROV_WORKER_ERR_INVALID_STATE;
    }
    return IOT_PROV_WORKER_OK;
}

iot_prov_worker_result_t iot_prov_worker_request_stop(
    iot_prov_worker_t *worker)
{
    if (worker == NULL || !worker->initialized) {
        return IOT_PROV_WORKER_ERR_INVALID_ARG;
    }
    if (worker->state != IOT_PROV_WORKER_STARTING &&
        worker->state != IOT_PROV_WORKER_RUNNING) {
        return IOT_PROV_WORKER_ERR_INVALID_STATE;
    }

    worker->state = IOT_PROV_WORKER_STOPPING;
    return IOT_PROV_WORKER_OK;
}

iot_prov_worker_result_t iot_prov_worker_note_stopped(
    iot_prov_worker_t *worker)
{
    if (worker == NULL || !worker->initialized) {
        return IOT_PROV_WORKER_ERR_INVALID_ARG;
    }
    if (worker->state != IOT_PROV_WORKER_STOPPING) {
        return IOT_PROV_WORKER_ERR_INVALID_STATE;
    }

    worker->state = IOT_PROV_WORKER_STOPPED;
    return IOT_PROV_WORKER_OK;
}

iot_prov_worker_state_t iot_prov_worker_get_state(
    const iot_prov_worker_t *worker)
{
    if (worker == NULL || !worker->initialized) {
        return IOT_PROV_WORKER_STOPPED;
    }
    return worker->state;
}

bool iot_prov_worker_generation_current(
    const iot_prov_worker_t *worker,
    uint32_t generation)
{
    return worker != NULL && worker->initialized && generation != 0U &&
           worker->generation == generation;
}
