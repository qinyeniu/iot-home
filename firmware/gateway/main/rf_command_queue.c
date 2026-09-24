// CQ-1: host-testable RF dispatch decisions plus a default-off FreeRTOS scaffold.
#include "rf_command_queue.h"

#include <string.h>

#ifndef RF_COMMAND_QUEUE_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#endif

static const int8_t s_supported_powers[] = {-10, 0, 8, 14, 18, 20};

bool rf_cq_request_id_valid(const char *request_id)
{
    size_t i;

    if (request_id == NULL) {
        return false;
    }
    for (i = 0; i < RF_CQ_REQUEST_ID_LEN; ++i) {
        char ch = request_id[i];
        bool digit = ch >= '0' && ch <= '9';
        bool lower_hex = ch >= 'a' && ch <= 'f';
        if (ch == '\0' || (!digit && !lower_hex)) {
            return false;
        }
    }
    return request_id[RF_CQ_REQUEST_ID_LEN] == '\0';
}

bool rf_cq_supported_power(int8_t power_dbm)
{
    size_t i;
    for (i = 0; i < sizeof(s_supported_powers) / sizeof(s_supported_powers[0]); ++i) {
        if (s_supported_powers[i] == power_dbm) {
            return true;
        }
    }
    return false;
}

rf_cq_result_t rf_cq_validate(const rf_dispatch_cmd_t *cmd)
{
    if (cmd == NULL || cmd->request_id[RF_CQ_REQUEST_ID_LEN] != '\0' ||
        !rf_cq_request_id_valid(cmd->request_id) ||
        cmd->source > RF_CQ_SOURCE_AUTO ||
        cmd->command < RF_CQ_COMMAND_QUERY ||
        cmd->command > RF_CQ_COMMAND_SET ||
        cmd->desired_mode > RF_CQ_MODE_MANUAL ||
        cmd->created_ms == 0 || cmd->deadline_ms == 0 ||
        (int32_t)(cmd->deadline_ms - cmd->created_ms) < 1000) {
        return RF_CQ_ERR_INVALID_ARG;
    }

    if (cmd->source == RF_CQ_SOURCE_USER) {
        if (!cmd->ignore_backoff) {
            return RF_CQ_ERR_INVALID_ARG;
        }
    } else {
        // Periodic reconcile and automatic power changes are background work:
        // they carry a concrete generation and must respect RF backoff.
        if (cmd->ignore_backoff ||
            cmd->policy_generation == RF_CQ_GENERATION_ANY) {
            return RF_CQ_ERR_INVALID_ARG;
        }
    }

    if (cmd->command == RF_CQ_COMMAND_QUERY) {
        if (cmd->desired_mode != RF_CQ_MODE_AUTO ||
            cmd->desired_power_dbm != 0) {
            return RF_CQ_ERR_INVALID_ARG;
        }
    } else if (!rf_cq_supported_power(cmd->desired_power_dbm)) {
        return RF_CQ_ERR_INVALID_ARG;
    }

    return RF_CQ_OK;
}

void rf_cq_fifo_init(rf_cq_fifo_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

uint8_t rf_cq_fifo_count(const rf_cq_fifo_t *queue)
{
    return queue == NULL ? 0 : queue->count;
}

const rf_dispatch_cmd_t *rf_cq_fifo_peek(const rf_cq_fifo_t *queue)
{
    if (queue == NULL || queue->count == 0) {
        return NULL;
    }
    return &queue->items[queue->head];
}

rf_cq_result_t rf_cq_fifo_push(rf_cq_fifo_t *queue,
                               const rf_dispatch_cmd_t *cmd)
{
    uint8_t slot;

    if (queue == NULL || rf_cq_validate(cmd) != RF_CQ_OK) {
        return RF_CQ_ERR_INVALID_ARG;
    }
    if (queue->count >= RF_CQ_CAPACITY) {
        return RF_CQ_ERR_FULL;
    }
    slot = (uint8_t)((queue->head + queue->count) % RF_CQ_CAPACITY);
    queue->items[slot] = *cmd;
    queue->count++;
    return RF_CQ_OK;
}

rf_cq_result_t rf_cq_fifo_pop(rf_cq_fifo_t *queue, rf_dispatch_cmd_t *cmd)
{
    if (queue == NULL || queue->count == 0) {
        return RF_CQ_ERR_INVALID_ARG;
    }
    if (cmd != NULL) {
        *cmd = queue->items[queue->head];
    }
    queue->head = (uint8_t)((queue->head + 1) % RF_CQ_CAPACITY);
    queue->count--;
    return RF_CQ_OK;
}

rf_cq_result_t rf_cq_submit_or_merge(rf_cq_fifo_t *queue,
                                     const rf_dispatch_cmd_t *cmd)
{
    uint8_t i;

    if (queue == NULL || rf_cq_validate(cmd) != RF_CQ_OK) {
        return RF_CQ_ERR_INVALID_ARG;
    }

    if (cmd->source != RF_CQ_SOURCE_USER) {
        for (i = 0; i < queue->count; ++i) {
            uint8_t slot = (uint8_t)((queue->head + i) % RF_CQ_CAPACITY);
            rf_dispatch_cmd_t *existing = &queue->items[slot];
            if (existing->source != RF_CQ_SOURCE_USER) {
                *existing = *cmd;
                return RF_CQ_OK;
            }
        }
    }

    return rf_cq_fifo_push(queue, cmd);
}

bool rf_cq_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

rf_cq_result_t rf_cq_evaluate(const rf_dispatch_cmd_t *cmd,
                              const rf_cq_context_t *context)
{
    rf_cq_result_t valid;

    if (context == NULL) {
        return RF_CQ_ERR_INVALID_ARG;
    }
    valid = rf_cq_validate(cmd);
    if (valid != RF_CQ_OK) {
        return valid;
    }
    if (rf_cq_time_reached(context->now_ms, cmd->deadline_ms)) {
        return RF_CQ_ERR_EXPIRED;
    }
    if (context->waiting_active &&
        !rf_cq_time_reached(context->now_ms, context->wait_deadline_ms)) {
        return RF_CQ_ERR_WAITING;
    }
    if (cmd->policy_generation != RF_CQ_GENERATION_ANY &&
        cmd->policy_generation != context->current_generation) {
        return RF_CQ_ERR_STALE_GENERATION;
    }
    // Zero means "no backoff configured". Without this special case a tick
    // near UINT32_MAX would make the signed subtraction treat zero as future.
    if (cmd->source != RF_CQ_SOURCE_USER &&
        !cmd->ignore_backoff &&
        context->backoff_until_ms != 0 &&
        !rf_cq_time_reached(context->now_ms, context->backoff_until_ms)) {
        return RF_CQ_ERR_BACKOFF;
    }
    return RF_CQ_OK;
}

#ifndef RF_COMMAND_QUEUE_HOST_TEST
static const char *const RF_CQ_TAG = "rf-cq";
static QueueHandle_t s_rf_cq_queue;
static TaskHandle_t s_rf_cq_task;

static void rf_cq_consumer_task(void *arg)
{
    rf_dispatch_cmd_t cmd;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_rf_cq_queue, &cmd, pdMS_TO_TICKS(60000)) == pdTRUE) {
            // CQ-1 only proves ownership scaffolding. Do not call any Zigbee
            // API until CQ-2/CQ-3 integrates reservation and waiting state.
            ESP_LOGI(RF_CQ_TAG,
                     "queued command id=%lu cmd=%u source=%u; not dispatched in CQ-1",
                     (unsigned long)cmd.id, (unsigned)cmd.command,
                     (unsigned)cmd.source);
        }
    }
}

bool rf_cq_is_started(void)
{
    return s_rf_cq_task != NULL;
}

esp_err_t rf_cq_start(void)
{
    BaseType_t task_result;

    if (s_rf_cq_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_rf_cq_queue = xQueueCreate(RF_CQ_CAPACITY, sizeof(rf_dispatch_cmd_t));
    if (s_rf_cq_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    task_result = xTaskCreate(rf_cq_consumer_task, "rf-dispatch",
                              4096, NULL, 5, &s_rf_cq_task);
    if (task_result != pdPASS) {
        s_rf_cq_task = NULL;
        vQueueDelete(s_rf_cq_queue);
        s_rf_cq_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t rf_cq_stop(void)
{
    if (s_rf_cq_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelete(s_rf_cq_task);
    s_rf_cq_task = NULL;
    vQueueDelete(s_rf_cq_queue);
    s_rf_cq_queue = NULL;
    return ESP_OK;
}

esp_err_t rf_cq_submit(const rf_dispatch_cmd_t *cmd)
{
    if (rf_cq_validate(cmd) != RF_CQ_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rf_cq_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_rf_cq_queue, cmd, 0) == pdTRUE ? ESP_OK
                                                       : ESP_ERR_NO_MEM;
}
#endif
