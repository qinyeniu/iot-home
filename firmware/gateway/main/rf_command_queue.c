// CQ-1: host-testable RF dispatch decisions plus a default-off FreeRTOS scaffold.
#include "rf_command_queue.h"

#include <string.h>

#ifndef RF_COMMAND_QUEUE_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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

static rf_cq_result_t rf_cq_build_user_base(uint32_t id,
                                            rf_cq_command_t command,
                                            rf_cq_mode_t desired_mode,
                                            int8_t desired_power_dbm,
                                            const char *request_id,
                                            uint32_t now_ms,
                                            rf_dispatch_cmd_t *cmd)
{
    if (cmd == NULL) {
        return RF_CQ_ERR_INVALID_ARG;
    }
    if (id == 0 || now_ms == 0 || !rf_cq_request_id_valid(request_id)) {
        return RF_CQ_ERR_INVALID_ARG;
    }
    if (!rf_cq_supported_power(desired_power_dbm)) {
        return RF_CQ_ERR_INVALID_ARG;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->id = id;
    cmd->source = RF_CQ_SOURCE_USER;
    cmd->command = command;
    cmd->desired_mode = desired_mode;
    cmd->desired_power_dbm = desired_power_dbm;
    cmd->policy_generation = RF_CQ_GENERATION_ANY;
    memcpy(cmd->request_id, request_id, RF_CQ_REQUEST_ID_LEN);
    cmd->request_id[RF_CQ_REQUEST_ID_LEN] = '\0';
    cmd->created_ms = now_ms;
    cmd->deadline_ms = now_ms + RF_CQ_USER_TTL_MS;
    cmd->ignore_backoff = true;
    return rf_cq_validate(cmd);
}

rf_cq_result_t rf_cq_build_user_query(uint32_t id, const char *request_id,
                                      uint32_t now_ms,
                                      rf_dispatch_cmd_t *cmd)
{
    return rf_cq_build_user_base(id, RF_CQ_COMMAND_QUERY, RF_CQ_MODE_AUTO,
                                 0, request_id, now_ms, cmd);
}

rf_cq_result_t rf_cq_build_user_manual_set(uint32_t id, int8_t power_dbm,
                                           const char *request_id,
                                           uint32_t now_ms,
                                           rf_dispatch_cmd_t *cmd)
{
    return rf_cq_build_user_base(id, RF_CQ_COMMAND_SET, RF_CQ_MODE_MANUAL,
                                 power_dbm, request_id, now_ms, cmd);
}

rf_cq_result_t rf_cq_build_user_auto_set(uint32_t id, int8_t power_dbm,
                                         const char *request_id,
                                         uint32_t now_ms,
                                         rf_dispatch_cmd_t *cmd)
{
    return rf_cq_build_user_base(id, RF_CQ_COMMAND_SET, RF_CQ_MODE_AUTO,
                                 power_dbm, request_id, now_ms, cmd);
}

static rf_cq_result_t rf_cq_build_background_base(
    uint32_t id, rf_cq_source_t source, rf_cq_command_t command,
    rf_cq_mode_t desired_mode, int8_t desired_power_dbm,
    uint32_t generation, uint32_t now_ms, rf_dispatch_cmd_t *cmd)
{
    if (cmd == NULL) {
        return RF_CQ_ERR_INVALID_ARG;
    }
    if (id == 0 || now_ms == 0 || source == RF_CQ_SOURCE_USER ||
        generation == 0 || generation == RF_CQ_GENERATION_ANY) {
        return RF_CQ_ERR_INVALID_ARG;
    }
    if (!rf_cq_supported_power(desired_power_dbm)) {
        return RF_CQ_ERR_INVALID_ARG;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->id = id;
    cmd->source = source;
    cmd->command = command;
    cmd->desired_mode = desired_mode;
    cmd->desired_power_dbm = desired_power_dbm;
    cmd->policy_generation = generation;
    cmd->created_ms = now_ms;
    cmd->deadline_ms = now_ms + RF_CQ_BACKGROUND_TTL_MS;
    cmd->ignore_backoff = false;
    return rf_cq_validate(cmd);
}

rf_cq_result_t rf_cq_build_periodic_query(uint32_t id, uint32_t generation,
                                          uint32_t now_ms,
                                          rf_dispatch_cmd_t *cmd)
{
    return rf_cq_build_background_base(id, RF_CQ_SOURCE_PERIODIC,
                                       RF_CQ_COMMAND_QUERY, RF_CQ_MODE_AUTO,
                                       0, generation, now_ms, cmd);
}

rf_cq_result_t rf_cq_build_periodic_set(uint32_t id, rf_cq_mode_t desired_mode,
                                        int8_t power_dbm, uint32_t generation,
                                        uint32_t now_ms,
                                        rf_dispatch_cmd_t *cmd)
{
    return rf_cq_build_background_base(id, RF_CQ_SOURCE_PERIODIC,
                                       RF_CQ_COMMAND_SET, desired_mode,
                                       power_dbm, generation, now_ms, cmd);
}

rf_cq_result_t rf_cq_build_auto_set(uint32_t id, int8_t power_dbm,
                                    uint32_t generation, uint32_t now_ms,
                                    rf_dispatch_cmd_t *cmd)
{
    return rf_cq_build_background_base(id, RF_CQ_SOURCE_AUTO,
                                       RF_CQ_COMMAND_SET, RF_CQ_MODE_AUTO,
                                       power_dbm, generation, now_ms, cmd);
}

rf_cq_result_t rf_cq_validate(const rf_dispatch_cmd_t *cmd)
{
    bool request_id_ok;

    if (cmd == NULL || cmd->request_id[RF_CQ_REQUEST_ID_LEN] != '\0' ||
        cmd->source > RF_CQ_SOURCE_AUTO ||
        cmd->command < RF_CQ_COMMAND_QUERY ||
        cmd->command > RF_CQ_COMMAND_SET ||
        cmd->desired_mode > RF_CQ_MODE_MANUAL ||
        cmd->created_ms == 0 || cmd->deadline_ms == 0 ||
        (int32_t)(cmd->deadline_ms - cmd->created_ms) < 1000) {
        return RF_CQ_ERR_INVALID_ARG;
    }

    request_id_ok = cmd->request_id[0] == '\0' ||
                    rf_cq_request_id_valid(cmd->request_id);
    if (cmd->source == RF_CQ_SOURCE_USER) {
        request_id_ok = rf_cq_request_id_valid(cmd->request_id);
    }
    if (!request_id_ok) {
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

typedef struct {
    QueueHandle_t queue;
    SemaphoreHandle_t stop_event;
    QueueSetHandle_t queue_set;
} rf_cq_worker_ctx_t;

static SemaphoreHandle_t s_state_mutex;
static StaticSemaphore_t s_state_mutex_buffer;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static rf_cq_worker_ctx_t s_worker_ctx;
static QueueHandle_t s_rf_cq_queue;
static SemaphoreHandle_t s_rf_cq_stop_event;
static QueueSetHandle_t s_rf_cq_queue_set;
static TaskHandle_t s_rf_cq_task;
static SemaphoreHandle_t s_rf_cq_done;
static bool s_stopping;
static rf_cq_dispatch_func_t s_dispatch_function;

static SemaphoreHandle_t rf_cq_state_lock(void)
{
    if (s_state_mutex == NULL) {
        portENTER_CRITICAL(&s_mutex_init_lock);
        if (s_state_mutex == NULL) {
            s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_buffer);
        }
        portEXIT_CRITICAL(&s_mutex_init_lock);
    }
    return s_state_mutex;
}

static void rf_cq_cleanup_start(QueueHandle_t queue,
                                SemaphoreHandle_t stop_event,
                                QueueSetHandle_t queue_set,
                                bool queue_added,
                                bool stop_added)
{
    if (queue_set != NULL) {
        if (stop_added) {
            xQueueRemoveFromSet(stop_event, queue_set);
        }
        if (queue_added) {
            xQueueRemoveFromSet(queue, queue_set);
        }
    }
    if (queue_set != NULL) {
        vQueueDelete((QueueHandle_t)queue_set);
    }
    if (stop_event != NULL) {
        vSemaphoreDelete(stop_event);
    }
    if (queue != NULL) {
        vQueueDelete(queue);
    }
}

static void rf_cq_consumer_task(void *arg)
{
    rf_cq_worker_ctx_t *ctx = (rf_cq_worker_ctx_t *)arg;
    rf_dispatch_cmd_t cmd;
    QueueSetMemberHandle_t member;

    for (;;) {
        member = xQueueSelectFromSet(ctx->queue_set, portMAX_DELAY);
        if (member == (QueueSetMemberHandle_t)ctx->stop_event) {
            (void)xSemaphoreTake(ctx->stop_event, 0);
            break;
        }
        if (member == (QueueSetMemberHandle_t)ctx->queue) {
            if (xQueueReceive(ctx->queue, &cmd, 0) == pdTRUE) {
                if (s_dispatch_function != NULL) {
                    // The configured integration owns reservation, Zigbee send
                    // and waiting-transaction installation for this one command.
                    s_dispatch_function(&cmd);
                } else {
                    // CQ-1 fallback: prove the queue path without calling Zigbee.
                    ESP_LOGI(RF_CQ_TAG,
                             "queued command id=%lu cmd=%u source=%u; no dispatch function",
                             (unsigned long)cmd.id, (unsigned)cmd.command,
                             (unsigned)cmd.source);
                }
            }
        }
    }

    /*
     * At this point the consumer no longer touches the queue, queue set, or
     * stop event. The completion semaphore is private to the stop transaction;
     * after giving it, the task deletes itself rather than being force-deleted
     * while it may still be inside a FreeRTOS call.
     */
    if (s_rf_cq_done != NULL) {
        xSemaphoreGive(s_rf_cq_done);
    }
    vTaskDelete(NULL);
}

bool rf_cq_is_started(void)
{
    bool started;
    SemaphoreHandle_t lock = rf_cq_state_lock();

    xSemaphoreTake(lock, portMAX_DELAY);
    started = s_rf_cq_task != NULL && !s_stopping;
    xSemaphoreGive(lock);
    return started;
}

esp_err_t rf_cq_configure_dispatch(rf_cq_dispatch_func_t func)
{
    SemaphoreHandle_t lock = rf_cq_state_lock();

    if (func == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_rf_cq_task != NULL || s_stopping) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_dispatch_function = func;
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t rf_cq_start(void)
{
    QueueHandle_t queue = NULL;
    SemaphoreHandle_t stop_event = NULL;
    QueueSetHandle_t queue_set = NULL;
    TaskHandle_t task = NULL;
    bool queue_added = false;
    bool stop_added = false;
    BaseType_t result;
    SemaphoreHandle_t lock = rf_cq_state_lock();

    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_rf_cq_task != NULL || s_stopping) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    queue = xQueueCreate(RF_CQ_CAPACITY, sizeof(rf_dispatch_cmd_t));
    if (queue == NULL) {
        xSemaphoreGive(lock);
        return ESP_ERR_NO_MEM;
    }

    stop_event = xSemaphoreCreateBinary();
    if (stop_event == NULL) {
        rf_cq_cleanup_start(queue, stop_event, queue_set,
                            queue_added, stop_added);
        xSemaphoreGive(lock);
        return ESP_ERR_NO_MEM;
    }

    queue_set = xQueueCreateSet(RF_CQ_CAPACITY + 1);
    if (queue_set == NULL) {
        rf_cq_cleanup_start(queue, stop_event, queue_set,
                            queue_added, stop_added);
        xSemaphoreGive(lock);
        return ESP_ERR_NO_MEM;
    }

    result = xQueueAddToSet(queue, queue_set);
    if (result != pdPASS) {
        rf_cq_cleanup_start(queue, stop_event, queue_set,
                            queue_added, stop_added);
        xSemaphoreGive(lock);
        return ESP_FAIL;
    }
    queue_added = true;

    result = xQueueAddToSet(stop_event, queue_set);
    if (result != pdPASS) {
        rf_cq_cleanup_start(queue, stop_event, queue_set,
                            queue_added, stop_added);
        xSemaphoreGive(lock);
        return ESP_FAIL;
    }
    stop_added = true;

    s_worker_ctx.queue = queue;
    s_worker_ctx.stop_event = stop_event;
    s_worker_ctx.queue_set = queue_set;

    result = xTaskCreate(rf_cq_consumer_task, "rf-dispatch", 4096,
                         &s_worker_ctx, 5, &task);
    if (result != pdPASS) {
        rf_cq_cleanup_start(queue, stop_event, queue_set,
                            queue_added, stop_added);
        xSemaphoreGive(lock);
        return ESP_ERR_NO_MEM;
    }

    s_rf_cq_queue = queue;
    s_rf_cq_stop_event = stop_event;
    s_rf_cq_queue_set = queue_set;
    s_rf_cq_task = task;
    s_stopping = false;
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t rf_cq_stop(void)
{
    SemaphoreHandle_t done;
    QueueHandle_t queue;
    SemaphoreHandle_t stop_event;
    QueueSetHandle_t queue_set;
    bool acknowledged = false;
    int wait_iteration;
    SemaphoreHandle_t lock = rf_cq_state_lock();

    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_rf_cq_task == NULL) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_stopping) {
        /*
         * Normally this branch is unreachable while an earlier stop is active
         * because this mutex is held for the whole wait. It remains a safe
         * recovery path after a previous stop timed out and released the lock.
         */
        if (s_rf_cq_done == NULL) {
            xSemaphoreGive(lock);
            return ESP_ERR_INVALID_STATE;
        }
        done = s_rf_cq_done;
        stop_event = s_rf_cq_stop_event;
    } else {
        done = xSemaphoreCreateBinary();
        if (done == NULL) {
            xSemaphoreGive(lock);
            return ESP_ERR_NO_MEM;
        }
        s_rf_cq_done = done;
        s_stopping = true;
        done = s_rf_cq_done;
        stop_event = s_rf_cq_stop_event;
    }

    queue = s_rf_cq_queue;
    queue_set = s_rf_cq_queue_set;

    /*
     * The worker does not acquire this state mutex, so it can wake from the
     * queue set even while stop holds the lock. Holding the lock here also
     * prevents submit/start from racing with queue deletion and serializes
     * concurrent stop callers.
     */
    (void)xSemaphoreGive(stop_event);

    for (wait_iteration = 0; wait_iteration < 50; ++wait_iteration) {
        if (xSemaphoreTake(done, pdMS_TO_TICKS(100))) {
            acknowledged = true;
            break;
        }
    }

    if (!acknowledged) {
        /*
         * Keep the stopping state latched and release the lock. A later
         * rf_cq_stop() can consume a late completion and clean up the owned
         * task/queue objects safely.
         */
        xSemaphoreGive(lock);
        return ESP_ERR_TIMEOUT;
    }

    s_rf_cq_queue = NULL;
    s_rf_cq_stop_event = NULL;
    s_rf_cq_queue_set = NULL;
    s_rf_cq_task = NULL;
    s_rf_cq_done = NULL;
    s_stopping = false;
    memset(&s_worker_ctx, 0, sizeof(s_worker_ctx));
    xSemaphoreGive(lock);

    vQueueDelete(queue);
    vSemaphoreDelete(stop_event);
    vQueueDelete((QueueHandle_t)queue_set);
    vSemaphoreDelete(done);
    return ESP_OK;
}

esp_err_t rf_cq_submit(const rf_dispatch_cmd_t *cmd)
{
    esp_err_t result;
    SemaphoreHandle_t lock = rf_cq_state_lock();

    if (rf_cq_validate(cmd) != RF_CQ_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * The same mutex is held across the non-blocking queue send. rf_cq_stop()
     * marks the stopping state and deletes the queue only after this operation
     * has completely returned.
     */
    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_stopping || s_rf_cq_task == NULL || s_rf_cq_queue == NULL) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    result = xQueueSend(s_rf_cq_queue, cmd, 0) == pdTRUE
                 ? ESP_OK
                 : ESP_ERR_NO_MEM;
    xSemaphoreGive(lock);
    return result;
}
#endif
