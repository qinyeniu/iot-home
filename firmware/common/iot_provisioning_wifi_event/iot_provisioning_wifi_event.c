/*
 * Provisioning Wi-Fi event adapter implementation.
 */

#include "iot_provisioning_wifi_event.h"

#include <string.h>

static iot_prov_error_t map_disconnect_reason(uint8_t reason)
{
    switch (reason) {
        case IOT_PROV_WIFI_REASON_NO_AP_FOUND:
        case IOT_PROV_WIFI_REASON_BEACON_TIMEOUT:
        case IOT_PROV_WIFI_REASON_ASSOC_FAIL:
        case IOT_PROV_WIFI_REASON_CONNECTION_FAIL:
            return IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT;

        case IOT_PROV_WIFI_REASON_AUTH_FAIL:
        case IOT_PROV_WIFI_REASON_HANDSHAKE_TIMEOUT:
            return IOT_PROV_ERR_WIFI_AUTH_FAILED;

        default:
            return IOT_PROV_ERR_INTERNAL;
    }
}

static bool input_valid(const iot_prov_wifi_input_t *input)
{
    if (input == NULL) {
        return false;
    }

    switch (input->type) {
        case IOT_PROV_WIFI_INPUT_CONNECTED:
        case IOT_PROV_WIFI_INPUT_GOT_IP:
            return true;

        case IOT_PROV_WIFI_INPUT_DISCONNECTED:
            return input->reason != 0U;
    }
    return false;
}

iot_prov_wifi_event_adapter_result_t iot_prov_wifi_event_build(
    const char *test_id,
    uint32_t owner_generation,
    const iot_prov_wifi_input_t *input,
    iot_prov_wifi_event_output_t *output)
{
    iot_prov_wifi_event_t event_type;
    iot_prov_command_result_t result;

    if (test_id == NULL || owner_generation == 0U || output == NULL ||
        !input_valid(input)) {
        return IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG;
    }

    memset(output, 0, sizeof(*output));
    if (input->type == IOT_PROV_WIFI_INPUT_GOT_IP) {
        event_type = IOT_PROV_WIFI_EVENT_GOT_IP;
        output->error = IOT_PROV_OK;
    } else if (input->type == IOT_PROV_WIFI_INPUT_CONNECTED) {
        event_type = IOT_PROV_WIFI_EVENT_CONNECTED;
        output->error = IOT_PROV_OK;
    } else {
        event_type = IOT_PROV_WIFI_EVENT_DISCONNECTED;
        output->error = map_disconnect_reason(input->reason);
    }

    result = iot_prov_make_wifi_event(test_id, owner_generation, event_type,
                                      &output->command);
    if (result != IOT_PROV_COMMAND_OK) {
        memset(output, 0, sizeof(*output));
        return IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG;
    }
    return IOT_PROV_WIFI_EVENT_ADAPTER_OK;
}

