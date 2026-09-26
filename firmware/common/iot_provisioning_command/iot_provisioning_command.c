/*
 * Provisioning command queue implementation.
 */

#include "iot_provisioning_command.h"

#include <stdio.h>
#include <string.h>

static size_t bounded_strlen(const char *value, size_t capacity)
{
    size_t len = 0U;

    if (value == NULL) {
        return capacity;
    }
    while (len < capacity && value[len] != '\0') {
        ++len;
    }
    return len;
}

static bool test_id_has_text(const char *test_id)
{
    size_t len = bounded_strlen(test_id, IOT_PROV_TEST_ID_MAX_LEN + 1U);

    return len > 0U && len <= IOT_PROV_TEST_ID_MAX_LEN;
}

static bool external_test_id_valid(
    iot_prov_command_type_t type,
    const char *test_id,
    size_t *length)
{
    size_t len;

    if (test_id == NULL) {
        len = 0U;
    } else {
        len = bounded_strlen(test_id, IOT_PROV_TEST_ID_MAX_LEN + 1U);
    }

    if (length != NULL) {
        *length = len;
    }
    if (len > IOT_PROV_TEST_ID_MAX_LEN) {
        return false;
    }

    switch (type) {
        case IOT_PROV_COMMAND_START_TEST:
        case IOT_PROV_COMMAND_WIFI_EVENT:
            return len > 0U;

        case IOT_PROV_COMMAND_CANCEL_TEST:
            return true;

        case IOT_PROV_COMMAND_TIMER_TICK:
            return len == 0U;
    }
    return false;
}

static iot_prov_command_result_t validate_command(
    const iot_prov_command_t *command)
{
    if (command == NULL) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }

    switch (command->type) {
        case IOT_PROV_COMMAND_START_TEST:
            if (!test_id_has_text(command->test_id) ||
                command->wifi_event != IOT_PROV_WIFI_EVENT_NONE) {
                return IOT_PROV_COMMAND_ERR_INVALID_ARG;
            }
            return IOT_PROV_COMMAND_OK;

        case IOT_PROV_COMMAND_CANCEL_TEST:
            if (bounded_strlen(command->test_id,
                               IOT_PROV_TEST_ID_MAX_LEN + 1U) >
                    IOT_PROV_TEST_ID_MAX_LEN ||
                command->wifi_event != IOT_PROV_WIFI_EVENT_NONE) {
                return IOT_PROV_COMMAND_ERR_INVALID_ARG;
            }
            return IOT_PROV_COMMAND_OK;

        case IOT_PROV_COMMAND_WIFI_EVENT:
            if (!test_id_has_text(command->test_id) ||
                command->owner_generation == 0U ||
                (command->wifi_event != IOT_PROV_WIFI_EVENT_DISCONNECTED &&
                 command->wifi_event != IOT_PROV_WIFI_EVENT_GOT_IP)) {
                return IOT_PROV_COMMAND_ERR_INVALID_ARG;
            }
            return IOT_PROV_COMMAND_OK;

        case IOT_PROV_COMMAND_TIMER_TICK:
            if (command->test_id[0] != '\0' ||
                command->owner_generation == 0U ||
                command->wifi_event != IOT_PROV_WIFI_EVENT_NONE) {
                return IOT_PROV_COMMAND_ERR_INVALID_ARG;
            }
            return IOT_PROV_COMMAND_OK;
    }
    return IOT_PROV_COMMAND_ERR_INVALID_ARG;
}

static void set_command(iot_prov_command_t *command,
                        iot_prov_command_type_t type,
                        const char *test_id,
                        uint32_t owner_generation,
                        iot_prov_wifi_event_t wifi_event)
{
    memset(command, 0, sizeof(*command));
    command->type = type;
    command->owner_generation = owner_generation;
    command->wifi_event = wifi_event;
    if (test_id != NULL) {
        (void)snprintf(command->test_id, sizeof(command->test_id), "%s",
                       test_id);
    }
}

iot_prov_command_result_t iot_prov_make_start_test(
    const char *test_id,
    uint32_t owner_generation,
    iot_prov_command_t *command)
{
    size_t id_len;

    if (command == NULL ||
        !external_test_id_valid(IOT_PROV_COMMAND_START_TEST, test_id,
                                &id_len)) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }
    set_command(command, IOT_PROV_COMMAND_START_TEST, test_id,
                owner_generation, IOT_PROV_WIFI_EVENT_NONE);
    return validate_command(command);
}

iot_prov_command_result_t iot_prov_make_cancel_test(
    const char *test_id,
    uint32_t owner_generation,
    iot_prov_command_t *command)
{
    size_t id_len;

    if (command == NULL ||
        !external_test_id_valid(IOT_PROV_COMMAND_CANCEL_TEST, test_id,
                                &id_len)) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }
    set_command(command, IOT_PROV_COMMAND_CANCEL_TEST, test_id,
                owner_generation, IOT_PROV_WIFI_EVENT_NONE);
    return validate_command(command);
}

iot_prov_command_result_t iot_prov_make_wifi_event(
    const char *test_id,
    uint32_t owner_generation,
    iot_prov_wifi_event_t wifi_event,
    iot_prov_command_t *command)
{
    size_t id_len;

    if (command == NULL ||
        !external_test_id_valid(IOT_PROV_COMMAND_WIFI_EVENT, test_id,
                                &id_len)) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }
    set_command(command, IOT_PROV_COMMAND_WIFI_EVENT, test_id,
                owner_generation, wifi_event);
    return validate_command(command);
}

iot_prov_command_result_t iot_prov_make_timer_tick(
    uint32_t owner_generation,
    iot_prov_command_t *command)
{
    if (command == NULL || owner_generation == 0U) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }
    set_command(command, IOT_PROV_COMMAND_TIMER_TICK, NULL,
                owner_generation, IOT_PROV_WIFI_EVENT_NONE);
    return validate_command(command);
}

void iot_prov_command_queue_init(iot_prov_command_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

uint8_t iot_prov_command_queue_count(const iot_prov_command_queue_t *queue)
{
    return queue == NULL ? 0U : queue->count;
}

bool iot_prov_command_queue_full(const iot_prov_command_queue_t *queue)
{
    return queue != NULL &&
           queue->count == IOT_PROV_COMMAND_QUEUE_CAPACITY;
}

iot_prov_command_result_t iot_prov_command_queue_push(
    iot_prov_command_queue_t *queue,
    const iot_prov_command_t *command)
{
    uint8_t slot;

    if (queue == NULL || validate_command(command) != IOT_PROV_COMMAND_OK) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }
    if (iot_prov_command_queue_full(queue)) {
        return IOT_PROV_COMMAND_ERR_FULL;
    }

    slot = (uint8_t)((uint8_t)(queue->head + queue->count) %
                     IOT_PROV_COMMAND_QUEUE_CAPACITY);
    queue->items[slot] = *command;
    ++queue->count;
    return IOT_PROV_COMMAND_OK;
}

iot_prov_command_result_t iot_prov_command_queue_peek(
    const iot_prov_command_queue_t *queue,
    iot_prov_command_t *command)
{
    if (queue == NULL || command == NULL) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }
    if (queue->count == 0U) {
        return IOT_PROV_COMMAND_ERR_INVALID_ARG;
    }

    *command = queue->items[queue->head];
    return IOT_PROV_COMMAND_OK;
}

iot_prov_command_result_t iot_prov_command_queue_pop(
    iot_prov_command_queue_t *queue,
    iot_prov_command_t *command)
{
    iot_prov_command_result_t result =
        iot_prov_command_queue_peek(queue, command);

    if (result != IOT_PROV_COMMAND_OK) {
        return result;
    }

    memset(&queue->items[queue->head], 0,
           sizeof(queue->items[queue->head]));
    queue->head = (uint8_t)((queue->head + 1U) %
                            IOT_PROV_COMMAND_QUEUE_CAPACITY);
    --queue->count;
    return IOT_PROV_COMMAND_OK;
}

void iot_prov_command_queue_clear(iot_prov_command_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

bool iot_prov_command_is_stale(
    const iot_prov_command_t *command,
    const char *active_test_id,
    uint32_t active_generation)
{
    const char *active_id = active_test_id == NULL ? "" : active_test_id;

    if (validate_command(command) != IOT_PROV_COMMAND_OK) {
        return true;
    }
    if (command->owner_generation != 0U && active_generation != 0U &&
        command->owner_generation != active_generation) {
        return true;
    }

    switch (command->type) {
        case IOT_PROV_COMMAND_START_TEST:
            return active_id[0] != '\0' &&
                   strcmp(command->test_id, active_id) != 0;

        case IOT_PROV_COMMAND_CANCEL_TEST:
            return command->test_id[0] != '\0' && active_id[0] != '\0' &&
                   strcmp(command->test_id, active_id) != 0;

        case IOT_PROV_COMMAND_WIFI_EVENT:
            return active_id[0] == '\0' ||
                   strcmp(command->test_id, active_id) != 0;

        case IOT_PROV_COMMAND_TIMER_TICK:
            return command->owner_generation != active_generation;
    }
    return true;
}
