// RF private command queue: CQ-1 offline scaffolding.
// Default-off in firmware; no existing gateway runtime path calls this module.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef RF_COMMAND_QUEUE_HOST_TEST
#include "esp_err.h"
#endif

#define RF_CQ_CAPACITY 8u
#define RF_CQ_REQUEST_ID_LEN 16u
#define RF_CQ_REQUEST_ID_SIZE 17u
#define RF_CQ_GENERATION_ANY UINT32_MAX

typedef enum {
    RF_CQ_SOURCE_USER = 0,
    RF_CQ_SOURCE_PERIODIC = 1,
    RF_CQ_SOURCE_AUTO = 2,
} rf_cq_source_t;

typedef enum {
    RF_CQ_COMMAND_QUERY = 1,
    RF_CQ_COMMAND_SET = 2,
} rf_cq_command_t;

typedef enum {
    RF_CQ_MODE_AUTO = 0,
    RF_CQ_MODE_MANUAL = 1,
} rf_cq_mode_t;

typedef enum {
    RF_CQ_OK = 0,
    RF_CQ_ERR_INVALID_ARG,
    RF_CQ_ERR_FULL,
    RF_CQ_ERR_EXPIRED,
    RF_CQ_ERR_STALE_GENERATION,
    RF_CQ_ERR_BACKOFF,
    RF_CQ_ERR_WAITING,
} rf_cq_result_t;

typedef struct {
    uint32_t id;
    rf_cq_source_t source;
    rf_cq_command_t command;
    rf_cq_mode_t desired_mode;
    int8_t desired_power_dbm;
    uint32_t policy_generation;
    char request_id[RF_CQ_REQUEST_ID_SIZE];
    uint32_t created_ms;
    uint32_t deadline_ms;
    bool ignore_backoff;
} rf_dispatch_cmd_t;

typedef struct {
    uint8_t count;
    uint8_t head;
    rf_dispatch_cmd_t items[RF_CQ_CAPACITY];
} rf_cq_fifo_t;

typedef struct {
    uint32_t now_ms;
    uint32_t current_generation;
    bool waiting_active;
    uint32_t wait_deadline_ms;
    uint32_t backoff_until_ms;
} rf_cq_context_t;

/*
 * Invoked by the single consumer for one dequeued command. The pointer is only
 * valid for the duration of the callback; callbacks must not retain it and must
 * not synchronously call rf_cq_stop().
 *
 * A configured function remains installed across a successful stop/restart and
 * may be replaced only while no consumer exists and no stop is in progress.
 * Passing NULL is not accepted; returning to the log-only CQ-1 path requires a
 * reboot. The callback must return promptly. Long Zigbee waits should be stored
 * as asynchronous state because a callback running beyond rf_cq_stop()'s bounded
 * wait makes that call return ESP_ERR_TIMEOUT.
 */
typedef void (*rf_cq_dispatch_func_t)(const rf_dispatch_cmd_t *cmd);

bool rf_cq_request_id_valid(const char *request_id);
bool rf_cq_supported_power(int8_t power_dbm);
rf_cq_result_t rf_cq_validate(const rf_dispatch_cmd_t *cmd);

void rf_cq_fifo_init(rf_cq_fifo_t *queue);
uint8_t rf_cq_fifo_count(const rf_cq_fifo_t *queue);
rf_cq_result_t rf_cq_fifo_push(rf_cq_fifo_t *queue, const rf_dispatch_cmd_t *cmd);
rf_cq_result_t rf_cq_fifo_pop(rf_cq_fifo_t *queue, rf_dispatch_cmd_t *cmd);
const rf_dispatch_cmd_t *rf_cq_fifo_peek(const rf_cq_fifo_t *queue);
rf_cq_result_t rf_cq_submit_or_merge(rf_cq_fifo_t *queue,
                                      const rf_dispatch_cmd_t *cmd);

bool rf_cq_time_reached(uint32_t now_ms, uint32_t deadline_ms);
rf_cq_result_t rf_cq_evaluate(const rf_dispatch_cmd_t *cmd,
                              const rf_cq_context_t *context);

#ifndef RF_COMMAND_QUEUE_HOST_TEST
esp_err_t rf_cq_configure_dispatch(rf_cq_dispatch_func_t func);
esp_err_t rf_cq_start(void);
esp_err_t rf_cq_stop(void);
esp_err_t rf_cq_submit(const rf_dispatch_cmd_t *cmd);
bool rf_cq_is_started(void);
#endif
