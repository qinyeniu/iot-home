/*
 * IoT-Home provisioning command queue.
 *
 * Host-compatible, RAM-only bounded FIFO used to serialize provisioning
 * commands. It does not call Wi-Fi APIs, start a task or write NVS.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "iot_provisioning_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_PROV_COMMAND_QUEUE_CAPACITY 2u

typedef enum {
    IOT_PROV_COMMAND_START_TEST = 1,
    IOT_PROV_COMMAND_CANCEL_TEST,
    IOT_PROV_COMMAND_WIFI_EVENT,
    IOT_PROV_COMMAND_TIMER_TICK,
} iot_prov_command_type_t;

typedef enum {
    IOT_PROV_WIFI_EVENT_NONE = 0,
    IOT_PROV_WIFI_EVENT_DISCONNECTED = 1,
    IOT_PROV_WIFI_EVENT_GOT_IP,
} iot_prov_wifi_event_t;

typedef enum {
    IOT_PROV_COMMAND_OK = 0,
    IOT_PROV_COMMAND_ERR_INVALID_ARG,
    IOT_PROV_COMMAND_ERR_FULL,
} iot_prov_command_result_t;

/*
 * RAM-only command queue. The object is not internally synchronized: the
 * integration task must be the only caller, or external callers must hold a
 * lock around push/peek/pop/clear.
 */
typedef struct {
    iot_prov_command_type_t type;
    iot_prov_wifi_event_t wifi_event;
    char test_id[IOT_PROV_TEST_ID_MAX_LEN + 1];

    /*
     * Zero is a wildcard only for externally submitted START/CANCEL commands.
     * WIFI_EVENT and TIMER_TICK must carry a concrete non-zero owner.
     */
    uint32_t owner_generation;
} iot_prov_command_t;

typedef struct {
    uint8_t head;
    uint8_t count;
    iot_prov_command_t items[IOT_PROV_COMMAND_QUEUE_CAPACITY];
} iot_prov_command_queue_t;

/* Command builders validate and zero-initialize each RAM-only command. */
iot_prov_command_result_t iot_prov_make_start_test(
    const char *test_id,
    uint32_t owner_generation,
    iot_prov_command_t *command);

iot_prov_command_result_t iot_prov_make_cancel_test(
    const char *test_id,
    uint32_t owner_generation,
    iot_prov_command_t *command);

iot_prov_command_result_t iot_prov_make_wifi_event(
    const char *test_id,
    uint32_t owner_generation,
    iot_prov_wifi_event_t wifi_event,
    iot_prov_command_t *command);

iot_prov_command_result_t iot_prov_make_timer_tick(
    uint32_t owner_generation,
    iot_prov_command_t *command);

void iot_prov_command_queue_init(iot_prov_command_queue_t *queue);
uint8_t iot_prov_command_queue_count(const iot_prov_command_queue_t *queue);
bool iot_prov_command_queue_full(const iot_prov_command_queue_t *queue);

iot_prov_command_result_t iot_prov_command_queue_push(
    iot_prov_command_queue_t *queue,
    const iot_prov_command_t *command);

iot_prov_command_result_t iot_prov_command_queue_peek(
    const iot_prov_command_queue_t *queue,
    iot_prov_command_t *command);

iot_prov_command_result_t iot_prov_command_queue_pop(
    iot_prov_command_queue_t *queue,
    iot_prov_command_t *command);

void iot_prov_command_queue_clear(iot_prov_command_queue_t *queue);

/*
 * Return true when a command belongs to an older test/owner and must be
 * discarded without changing the provisioning session.
 */
bool iot_prov_command_is_stale(
    const iot_prov_command_t *command,
    const char *active_test_id,
    uint32_t active_generation);

#ifdef __cplusplus
}
#endif
