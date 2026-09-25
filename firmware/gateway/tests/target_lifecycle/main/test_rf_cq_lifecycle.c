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

    if (s_failures != 0) {
        ESP_LOGE(TAG, "RF command queue lifecycle tests: FAIL (%d)", s_failures);
        abort();
    }
    ESP_LOGI(TAG, "RF command queue lifecycle tests: PASS");
}
