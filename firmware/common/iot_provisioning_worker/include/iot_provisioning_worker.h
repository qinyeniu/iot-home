/*
 * IoT-Home provisioning worker lifecycle.
 *
 * Host-compatible RAM-only state machine used by the future provisioning
 * task. It does not create tasks, call Wi-Fi/MQTT APIs or write NVS.
 *
 * The object is not internally synchronized. The integration layer must hold
 * the provisioning lock around every call, including state/generation reads.
 * FreeRTOS task-handle lifetime and task self-deletion synchronization are
 * handled separately by the integration layer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IOT_PROV_WORKER_STOPPED = 0,
    IOT_PROV_WORKER_STARTING,
    IOT_PROV_WORKER_RUNNING,
    IOT_PROV_WORKER_STOPPING,
} iot_prov_worker_state_t;

typedef struct {
    bool initialized;
    iot_prov_worker_state_t state;
    uint32_t generation;
} iot_prov_worker_t;

typedef enum {
    IOT_PROV_WORKER_OK = 0,
    IOT_PROV_WORKER_ERR_INVALID_ARG,
    IOT_PROV_WORKER_ERR_INVALID_STATE,
} iot_prov_worker_result_t;

iot_prov_worker_result_t iot_prov_worker_init(iot_prov_worker_t *worker);

/* STOPPED -> STARTING and allocate a new non-zero lifecycle generation. */
iot_prov_worker_result_t iot_prov_worker_request_start(
    iot_prov_worker_t *worker);

/*
 * Mark the task running. If stop was requested before the task entry ran,
 * remain in STOPPING so the task can exit immediately.
 */
iot_prov_worker_result_t iot_prov_worker_note_running(
    iot_prov_worker_t *worker);

/* Request stop from STARTING or RUNNING. */
iot_prov_worker_result_t iot_prov_worker_request_stop(
    iot_prov_worker_t *worker);

/* STOPPING -> STOPPED when the task is about to delete itself. */
iot_prov_worker_result_t iot_prov_worker_note_stopped(
    iot_prov_worker_t *worker);

iot_prov_worker_state_t iot_prov_worker_get_state(
    const iot_prov_worker_t *worker);

bool iot_prov_worker_generation_current(
    const iot_prov_worker_t *worker,
    uint32_t generation);

#ifdef __cplusplus
}
#endif

