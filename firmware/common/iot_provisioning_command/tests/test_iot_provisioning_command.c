#include "iot_provisioning_command.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(condition) do {                                         \
    if (!(condition)) {                                               \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,    \
               __LINE__);                                             \
        ++s_failures;                                                 \
    }                                                                 \
} while (0)

static void test_builders(void)
{
    iot_prov_command_t command;

    CHECK(iot_prov_make_start_test("tmp-0001", 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_START_TEST);
    CHECK(strcmp(command.test_id, "tmp-0001") == 0);
    CHECK(command.owner_generation == 0U);

    CHECK(iot_prov_make_start_test(NULL, 0U, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
    CHECK(iot_prov_make_start_test("tmp-0001", 0U, NULL) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);

    CHECK(iot_prov_make_cancel_test(NULL, 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_CANCEL_TEST);
    CHECK(command.test_id[0] == '\0');

    CHECK(iot_prov_make_wifi_event("tmp-0001", 7U,
                                    IOT_PROV_WIFI_EVENT_GOT_IP, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.wifi_event == IOT_PROV_WIFI_EVENT_GOT_IP);
    CHECK(iot_prov_make_wifi_event("tmp-0001", 0U,
                                    IOT_PROV_WIFI_EVENT_GOT_IP, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
    CHECK(iot_prov_make_wifi_event(NULL, 7U,
                                    IOT_PROV_WIFI_EVENT_GOT_IP, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
    CHECK(iot_prov_make_wifi_event("tmp-0001", 7U, 0, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);

    CHECK(iot_prov_make_timer_tick(7U, &command) == IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_TIMER_TICK);
    CHECK(command.owner_generation == 7U);
    CHECK(iot_prov_make_timer_tick(0U, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
}


static void test_test_id_length_boundaries(void)
{
    iot_prov_command_t command;
    const char *id32 = "tmp-1234567890123456789012345678";
    const char *id33 = "tmp-12345678901234567890123456789";

    CHECK(strlen(id32) == 32U);
    CHECK(strlen(id33) == 33U);

    CHECK(iot_prov_make_start_test(id32, 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(strlen(command.test_id) == 32U);
    CHECK(iot_prov_make_start_test(id33, 0U, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);

    CHECK(iot_prov_make_cancel_test(id32, 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(strlen(command.test_id) == 32U);
    CHECK(iot_prov_make_cancel_test(id33, 0U, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);

    CHECK(iot_prov_make_wifi_event(id32, 3U, IOT_PROV_WIFI_EVENT_GOT_IP,
                                    &command) == IOT_PROV_COMMAND_OK);
    CHECK(strlen(command.test_id) == 32U);
    CHECK(iot_prov_make_wifi_event(id33, 3U, IOT_PROV_WIFI_EVENT_GOT_IP,
                                    &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
}

static void test_wifi_event_subtypes(void)
{
    iot_prov_command_t command;

    CHECK(iot_prov_make_wifi_event("tmp-0001", 3U,
                                    IOT_PROV_WIFI_EVENT_DISCONNECTED,
                                    &command) == IOT_PROV_COMMAND_OK);
    CHECK(command.wifi_event == IOT_PROV_WIFI_EVENT_DISCONNECTED);
    CHECK(iot_prov_make_wifi_event("tmp-0001", 3U,
                                    IOT_PROV_WIFI_EVENT_GOT_IP,
                                    &command) == IOT_PROV_COMMAND_OK);
    CHECK(command.wifi_event == IOT_PROV_WIFI_EVENT_GOT_IP);
    CHECK(iot_prov_make_wifi_event("tmp-0001", 3U,
                                    IOT_PROV_WIFI_EVENT_NONE,
                                    &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
    CHECK(iot_prov_make_wifi_event("tmp-0001", 3U,
                                    (iot_prov_wifi_event_t)99,
                                    &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
}

static void test_timer_stale_after_owner_exit(void)
{
    iot_prov_command_t command;

    CHECK(iot_prov_make_timer_tick(5U, &command) == IOT_PROV_COMMAND_OK);
    CHECK(!iot_prov_command_is_stale(&command, "tmp-0001", 5U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0001", 4U));
    CHECK(iot_prov_command_is_stale(&command, "", 0U));
}

static void test_interleaved_ring_reuse_and_cleared_slot(void)
{
    iot_prov_command_queue_t queue;
    iot_prov_command_t command;
    const unsigned char *popped_slot;

    iot_prov_command_queue_init(&queue);
    CHECK(iot_prov_make_start_test("tmp-A", 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_make_timer_tick(5U, &command) == IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_OK);

    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_START_TEST);
    popped_slot = (const unsigned char *)&queue.items[queue.head == 0U ? 1U : 0U];
    for (size_t i = 0U; i < sizeof(queue.items[0]); ++i) {
        CHECK(popped_slot[i] == 0U);
    }

    CHECK(iot_prov_make_wifi_event("tmp-C", 5U,
                                    IOT_PROV_WIFI_EVENT_GOT_IP, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_OK);

    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_TIMER_TICK);
    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_WIFI_EVENT);
    CHECK(strcmp(command.test_id, "tmp-C") == 0);
}

static void test_fifo_order_full_and_wrapping(void)
{
    iot_prov_command_queue_t queue;
    iot_prov_command_t command;

    iot_prov_command_queue_init(&queue);
    CHECK(iot_prov_command_queue_count(&queue) == 0U);
    CHECK(!iot_prov_command_queue_full(&queue));
    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);

    CHECK(iot_prov_make_start_test("tmp-0001", 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_make_timer_tick(3U, &command) == IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_count(&queue) == 2U);
    CHECK(iot_prov_command_queue_full(&queue));
    CHECK(iot_prov_make_cancel_test("tmp-0001", 3U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_ERR_FULL);

    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_START_TEST);
    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_TIMER_TICK);
    CHECK(iot_prov_command_queue_count(&queue) == 0U);

    /* Verify the two-slot ring can wrap and reuse the first slot. */
    CHECK(iot_prov_make_wifi_event("tmp-0002", 4U,
                                    IOT_PROV_WIFI_EVENT_GOT_IP, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_peek(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(command.type == IOT_PROV_COMMAND_WIFI_EVENT);
    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_count(&queue) == 0U);
}

static void test_stale_detection(void)
{
    iot_prov_command_t command;

    CHECK(iot_prov_make_start_test("tmp-0001", 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(!iot_prov_command_is_stale(&command, "tmp-0001", 0U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0002", 0U));

    CHECK(iot_prov_make_cancel_test("tmp-0001", 5U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(!iot_prov_command_is_stale(&command, "tmp-0001", 5U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0002", 5U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0001", 6U));

    CHECK(iot_prov_make_cancel_test(NULL, 5U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(!iot_prov_command_is_stale(&command, "tmp-0001", 5U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0001", 6U));

    CHECK(iot_prov_make_wifi_event("tmp-0001", 5U,
                                    IOT_PROV_WIFI_EVENT_GOT_IP, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(!iot_prov_command_is_stale(&command, "tmp-0001", 5U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0002", 5U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0001", 6U));
    CHECK(iot_prov_command_is_stale(&command, "", 5U));

    CHECK(iot_prov_make_timer_tick(5U, &command) == IOT_PROV_COMMAND_OK);
    CHECK(!iot_prov_command_is_stale(&command, "tmp-0001", 5U));
    CHECK(iot_prov_command_is_stale(&command, "tmp-0001", 6U));

    command.type = (iot_prov_command_type_t)99;
    CHECK(iot_prov_command_is_stale(&command, "tmp-0001", 5U));
}

static void test_clear_queue(void)
{
    iot_prov_command_queue_t queue;
    iot_prov_command_t command;

    iot_prov_command_queue_init(&queue);
    CHECK(iot_prov_make_start_test("tmp-0001", 0U, &command) ==
          IOT_PROV_COMMAND_OK);
    CHECK(iot_prov_command_queue_push(&queue, &command) ==
          IOT_PROV_COMMAND_OK);
    iot_prov_command_queue_clear(&queue);
    CHECK(iot_prov_command_queue_count(&queue) == 0U);
    CHECK(iot_prov_command_queue_pop(&queue, &command) ==
          IOT_PROV_COMMAND_ERR_INVALID_ARG);
}

int main(void)
{
    test_builders();
    test_fifo_order_full_and_wrapping();
    test_stale_detection();
    test_clear_queue();
    test_test_id_length_boundaries();
    test_wifi_event_subtypes();
    test_timer_stale_after_owner_exit();
    test_interleaved_ring_reuse_and_cleared_slot();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all provisioning command queue checks passed\n");
    return 0;
}
