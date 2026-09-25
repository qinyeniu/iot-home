#include "rf_command_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "rf-cq-test";
static int s_failures;

#define CHECK(condition) do {                                         \
    if (!(condition)) {                                               \
        ESP_LOGE(TAG, "CHECK failed: %s @ %s:%d", #condition,         \
                 __FILE__, __LINE__);                                 \
        ++s_failures;                                                \
    }                                                                \
} while (0)

static rf_dispatch_cmd_t make_user_query(void)
{
    rf_dispatch_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.id = 1;
    cmd.source = RF_CQ_SOURCE_USER;
    cmd.command = RF_CQ_COMMAND_QUERY;
    cmd.desired_mode = RF_CQ_MODE_AUTO;
    cmd.desired_power_dbm = 0;
    cmd.policy_generation = RF_CQ_GENERATION_ANY;
    cmd.created_ms = (uint32_t)esp_timer_get_time() / 1000;
    if (cmd.created_ms == 0) {
        cmd.created_ms = 1;
    }
    cmd.deadline_ms = cmd.created_ms + 5000;
    cmd.ignore_backoff = true;
    memcpy(cmd.request_id, "0123456789abcdef", sizeof(cmd.request_id) - 1);
    return cmd;
}

static rf_dispatch_cmd_t make_periodic_query(uint32_t generation)
{
    rf_dispatch_cmd_t cmd = make_user_query();

    cmd.id = 2;
    cmd.source = RF_CQ_SOURCE_PERIODIC;
    cmd.policy_generation = generation;
    cmd.ignore_backoff = false;
    return cmd;
}

static TaskHandle_t s_test_task;
static volatile uint32_t s_callback_count;
static volatile uint32_t s_last_callback_id;
static volatile uint32_t s_last_callback_command;
static volatile uint32_t s_last_callback_source;

static void test_dispatch_callback(const rf_dispatch_cmd_t *cmd)
{
    s_last_callback_id = cmd->id;
    s_last_callback_command = cmd->command;
    s_last_callback_source = cmd->source;
    ++s_callback_count;
    if (s_test_task != NULL) {
        xTaskNotifyGive(s_test_task);
    }
}

static void test_dispatch_callback_invoked(void)
{
    rf_dispatch_cmd_t user = make_user_query();

    s_test_task = xTaskGetCurrentTaskHandle();
    s_callback_count = 0;
    s_last_callback_id = 0;
    s_last_callback_command = 0;
    s_last_callback_source = 0;

    CHECK(rf_cq_configure_dispatch(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(rf_cq_configure_dispatch(test_dispatch_callback) == ESP_OK);
    CHECK(rf_cq_start() == ESP_OK);
    CHECK(rf_cq_configure_dispatch(test_dispatch_callback) == ESP_ERR_INVALID_STATE);
    CHECK(rf_cq_submit(&user) == ESP_OK);
    CHECK(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 1);
    CHECK(s_callback_count == 1);
    CHECK(s_last_callback_id == 1);
    CHECK(s_last_callback_command == RF_CQ_COMMAND_QUERY);
    CHECK(s_last_callback_source == RF_CQ_SOURCE_USER);
    CHECK(rf_cq_stop() == ESP_OK);
    // The integration function is retained across a successful stop and can be
    // replaced before another consumer is started.
    CHECK(rf_cq_configure_dispatch(test_dispatch_callback) == ESP_OK);
}

static void test_idle_start_stop(void)
{
    CHECK(rf_cq_start() == ESP_OK);
    CHECK(rf_cq_is_started());
    CHECK(rf_cq_start() == ESP_ERR_INVALID_STATE);
    vTaskDelay(pdMS_TO_TICKS(100));
    CHECK(rf_cq_stop() == ESP_OK);
    CHECK(!rf_cq_is_started());
    CHECK(rf_cq_stop() == ESP_ERR_INVALID_STATE);
    CHECK(rf_cq_start() == ESP_OK);
    CHECK(rf_cq_stop() == ESP_OK);
}

static void test_submit_before_stop(void)
{
    rf_dispatch_cmd_t user = make_user_query();
    rf_dispatch_cmd_t periodic = make_periodic_query(10);

    CHECK(rf_cq_start() == ESP_OK);
    CHECK(rf_cq_submit(&user) == ESP_OK);
    CHECK(rf_cq_submit(&periodic) == ESP_OK);
    CHECK(rf_cq_submit(NULL) == ESP_ERR_INVALID_ARG);
    vTaskDelay(pdMS_TO_TICKS(300));
    CHECK(rf_cq_stop() == ESP_OK);
    CHECK(!rf_cq_is_started());
}

void app_main(void)
{
    test_idle_start_stop();
    test_submit_before_stop();
    test_dispatch_callback_invoked();

    if (s_failures != 0) {
        ESP_LOGE(TAG, "RF command queue lifecycle tests: FAIL (%d)", s_failures);
        abort();
    }
    ESP_LOGI(TAG, "RF command queue lifecycle tests: PASS");
}
