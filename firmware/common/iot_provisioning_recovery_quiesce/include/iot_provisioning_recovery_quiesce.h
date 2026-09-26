/*
 * IoT-Home normal Wi-Fi recovery quiesce coordinator.
 *
 * Host-compatible RAM-only policy used by the future provisioning flow to
 * prevent owner acquisition while a long normal recovery operation (notably a
 * blocking RF health scan) or its destructive tail is in flight. It does not
 * call Wi-Fi APIs, create tasks or write NVS.
 *
 * The object is not internally synchronized. The integration layer must hold
 * the provisioning lock around every call, including reads.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool operation_active;
    bool destructive_tail_active;
    bool quiesce_requested;
    uint32_t operation_id;
    uint32_t destructive_tail_id;
    uint32_t last_operation_id;
    uint32_t quiesce_request_id;
} iot_recovery_quiesce_t;

typedef enum {
    IOT_RECOVERY_QUIESCE_OK = 0,
    IOT_RECOVERY_QUIESCE_BUSY,
    IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG,
    IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE,
} iot_recovery_quiesce_result_t;

iot_recovery_quiesce_result_t iot_recovery_quiesce_init(
    iot_recovery_quiesce_t *state);

/*
 * Register one normal recovery operation before a blocking scan/connect or a
 * destructive stop/start sequence. Fails while quiesce has been requested.
 * operation_id must be non-zero, strictly monotonic and never reused during
 * this boot/RAM lifetime.
 */
iot_recovery_quiesce_result_t iot_recovery_quiesce_begin_operation(
    iot_recovery_quiesce_t *state,
    uint32_t operation_id);

/*
 * Finish an operation. When request_destructive_tail is true and no quiesce
 * request is present, atomically enter destructive-tail-active state. The
 * output is true only when that tail has been claimed. The caller must then
 * perform the destructive action immediately; a non-resetting action must be
 * completed with end_destructive_tail().
 */
iot_recovery_quiesce_result_t iot_recovery_quiesce_end_operation(
    iot_recovery_quiesce_t *state,
    uint32_t operation_id,
    bool request_destructive_tail,
    bool *destructive_tail_active);

/*
 * Finish a non-resetting destructive tail. Deep sleep does not return and
 * therefore does not need this call. If a quiesce request arrived during the
 * tail, that request remains armed after this call.
 */
iot_recovery_quiesce_result_t iot_recovery_quiesce_end_destructive_tail(
    iot_recovery_quiesce_t *state,
    uint32_t tail_id);

/*
 * Ask normal recovery to become quiet. Returns OK when no operation or
 * destructive tail is active; returns BUSY otherwise. Repeating the same
 * request after the operation/tail ends returns OK and the provisioning owner
 * may be acquired.
 */
iot_recovery_quiesce_result_t iot_recovery_quiesce_request(
    iot_recovery_quiesce_t *state,
    uint32_t request_id);

/* Cancel a pending quiesce request if owner acquisition was abandoned. */
iot_recovery_quiesce_result_t iot_recovery_quiesce_cancel_request(
    iot_recovery_quiesce_t *state,
    uint32_t request_id);

/* Reset to normal recovery when provisioning ends and ownership returns. */
iot_recovery_quiesce_result_t iot_recovery_quiesce_reset(
    iot_recovery_quiesce_t *state);

bool iot_recovery_quiesce_operation_active(
    const iot_recovery_quiesce_t *state);

bool iot_recovery_quiesce_destructive_tail_active(
    const iot_recovery_quiesce_t *state);

bool iot_recovery_quiesce_is_clear(
    const iot_recovery_quiesce_t *state);

#ifdef __cplusplus
}
#endif
