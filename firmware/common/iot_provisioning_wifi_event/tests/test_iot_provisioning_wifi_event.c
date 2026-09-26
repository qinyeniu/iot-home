#include "iot_provisioning_wifi_event.h"

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

static void test_got_ip(void)
{
    iot_prov_wifi_input_t input = {
        .type = IOT_PROV_WIFI_INPUT_GOT_IP,
    };
    iot_prov_wifi_event_output_t output;

    CHECK(iot_prov_wifi_event_build("tmp-0001", 1U, &input, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_OK);
    CHECK(output.error == IOT_PROV_OK);
    CHECK(output.command.type == IOT_PROV_COMMAND_WIFI_EVENT);
    CHECK(output.command.wifi_event == IOT_PROV_WIFI_EVENT_GOT_IP);
    CHECK(output.command.owner_generation == 1U);
    CHECK(strcmp(output.command.test_id, "tmp-0001") == 0);
}

static void test_disconnect_mapping(void)
{
    static const struct {
        uint8_t reason;
        iot_prov_error_t expected;
    } cases[] = {
        { IOT_PROV_WIFI_REASON_NO_AP_FOUND,
          IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT },
        { IOT_PROV_WIFI_REASON_BEACON_TIMEOUT,
          IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT },
        { IOT_PROV_WIFI_REASON_ASSOC_FAIL,
          IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT },
        { IOT_PROV_WIFI_REASON_CONNECTION_FAIL,
          IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT },
        { IOT_PROV_WIFI_REASON_AUTH_FAIL,
          IOT_PROV_ERR_WIFI_AUTH_FAILED },
        { IOT_PROV_WIFI_REASON_HANDSHAKE_TIMEOUT,
          IOT_PROV_ERR_WIFI_AUTH_FAILED },
        { 1U, IOT_PROV_ERR_INTERNAL },
        { 255U, IOT_PROV_ERR_INTERNAL },
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        iot_prov_wifi_input_t input = {
            .type = IOT_PROV_WIFI_INPUT_DISCONNECTED,
            .reason = cases[index].reason,
        };
        iot_prov_wifi_event_output_t output;

        CHECK(iot_prov_wifi_event_build("tmp-0001", 1U, &input, &output) ==
              IOT_PROV_WIFI_EVENT_ADAPTER_OK);
        CHECK(output.error == cases[index].expected);
        CHECK(output.command.type == IOT_PROV_COMMAND_WIFI_EVENT);
        CHECK(output.command.wifi_event == IOT_PROV_WIFI_EVENT_DISCONNECTED);
        CHECK(output.command.owner_generation == 1U);
    }
}

static void test_invalid_args(void)
{
    iot_prov_wifi_input_t got_ip = {
        .type = IOT_PROV_WIFI_INPUT_GOT_IP,
    };
    iot_prov_wifi_input_t disconnected = {
        .type = IOT_PROV_WIFI_INPUT_DISCONNECTED,
        .reason = IOT_PROV_WIFI_REASON_AUTH_FAIL,
    };
    iot_prov_wifi_input_t bad_type = {
        .type = (iot_prov_wifi_input_type_t)99,
    };
    iot_prov_wifi_input_t zero_reason = {
        .type = IOT_PROV_WIFI_INPUT_DISCONNECTED,
        .reason = 0U,
    };
    iot_prov_wifi_event_output_t output;

    CHECK(iot_prov_wifi_event_build(NULL, 1U, &got_ip, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_event_build("", 1U, &got_ip, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_event_build("tmp-0001", 0U, &got_ip, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_event_build("tmp-0001", 1U, NULL, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_event_build("tmp-0001", 1U, &got_ip, NULL) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_event_build("tmp-0001", 1U, &bad_type, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_event_build("tmp-0001", 1U, &zero_reason, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_event_build(
              "tmp-12345678901234567890123456789", 1U,
              &disconnected, &output) ==
          IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG);
}

int main(void)
{
    test_got_ip();
    test_disconnect_mapping();
    test_invalid_args();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all provisioning wifi event adapter checks passed\n");
    return 0;
}
