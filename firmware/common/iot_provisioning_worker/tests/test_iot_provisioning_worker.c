#include "iot_provisioning_worker.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

#define CHECK(condition) do {                                         \
    if (!(condition)) {                                               \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,    \
               __LINE__);                                             \
        ++s_failures;                                                 \
    }                                                                 \
} while (0)

static void test_initial_state(void)
{
    iot_prov_worker_t worker;

    CHECK(iot_prov_worker_init(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_STOPPED);
    CHECK(!iot_prov_worker_generation_current(&worker, 0U));
    CHECK(!iot_prov_worker_generation_current(&worker, 1U));
    CHECK(iot_prov_worker_init(NULL) == IOT_PROV_WORKER_ERR_INVALID_ARG);
}

static void test_normal_lifecycle(void)
{
    iot_prov_worker_t worker;

    CHECK(iot_prov_worker_init(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_request_start(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_STARTING);
    CHECK(iot_prov_worker_generation_current(&worker, 1U));
    CHECK(iot_prov_worker_note_running(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_RUNNING);
    CHECK(iot_prov_worker_request_stop(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_STOPPING);
    CHECK(iot_prov_worker_note_stopped(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_STOPPED);

    CHECK(iot_prov_worker_request_start(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_generation_current(&worker, 2U));
    CHECK(iot_prov_worker_note_stopped(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);
}

static void test_early_stop_before_task_entry(void)
{
    iot_prov_worker_t worker;

    CHECK(iot_prov_worker_init(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_request_start(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_request_stop(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_STOPPING);

    /* The task finally enters and must observe that stop already won. */
    CHECK(iot_prov_worker_note_running(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_STOPPING);
    CHECK(iot_prov_worker_note_stopped(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_get_state(&worker) == IOT_PROV_WORKER_STOPPED);
}

static void test_invalid_transitions_and_args(void)
{
    iot_prov_worker_t worker;

    CHECK(iot_prov_worker_init(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_request_stop(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);
    CHECK(iot_prov_worker_note_running(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);
    CHECK(iot_prov_worker_note_stopped(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);

    CHECK(iot_prov_worker_request_start(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_request_start(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);
    CHECK(iot_prov_worker_note_stopped(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);

    CHECK(iot_prov_worker_note_running(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_request_start(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);
    CHECK(iot_prov_worker_note_running(&worker) ==
          IOT_PROV_WORKER_ERR_INVALID_STATE);

    CHECK(iot_prov_worker_request_stop(NULL) ==
          IOT_PROV_WORKER_ERR_INVALID_ARG);
    CHECK(iot_prov_worker_note_running(NULL) ==
          IOT_PROV_WORKER_ERR_INVALID_ARG);
}

static void test_generation_wrap(void)
{
    iot_prov_worker_t worker;

    CHECK(iot_prov_worker_init(&worker) == IOT_PROV_WORKER_OK);
    worker.generation = UINT32_MAX;
    CHECK(iot_prov_worker_request_start(&worker) == IOT_PROV_WORKER_OK);
    CHECK(iot_prov_worker_generation_current(&worker, 1U));
}

int main(void)
{
    test_initial_state();
    test_normal_lifecycle();
    test_early_stop_before_task_entry();
    test_invalid_transitions_and_args();
    test_generation_wrap();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all provisioning worker lifecycle checks passed\n");
    return 0;
}

