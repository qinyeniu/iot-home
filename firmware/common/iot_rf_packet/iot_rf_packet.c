#include "iot_rf_packet.h"

#include <string.h>

size_t iot_rf_packet_wire_length(uint8_t version)
{
    if (version == IOT_RF_PACKET_V1_VERSION) {
        return IOT_RF_PACKET_V1_LEN;
    }
    if (version == IOT_RF_PACKET_V2_VERSION) {
        return IOT_RF_PACKET_V2_LEN;
    }
    return 0U;
}

bool iot_rf_packet_command_is_valid(uint8_t command)
{
    return command == IOT_RF_CMD_QUERY ||
           command == IOT_RF_CMD_SET ||
           command == IOT_RF_CMD_REPORT;
}

bool iot_rf_packet_mode_is_valid(uint8_t mode)
{
    return mode == IOT_RF_MODE_AUTO || mode == IOT_RF_MODE_MANUAL;
}

bool iot_rf_packet_decode(const uint8_t *data,
                          size_t data_len,
                          iot_rf_packet_t *packet)
{
    uint8_t version;

    if (data == NULL || packet == NULL) {
        return false;
    }
    if (data_len != IOT_RF_PACKET_V1_LEN &&
        data_len != IOT_RF_PACKET_V2_LEN) {
        return false;
    }
    if (data[0] != IOT_RF_PACKET_MAGIC0 ||
        data[1] != IOT_RF_PACKET_MAGIC1) {
        return false;
    }

    version = data[2];
    if ((version == IOT_RF_PACKET_V1_VERSION &&
         data_len != IOT_RF_PACKET_V1_LEN) ||
        (version == IOT_RF_PACKET_V2_VERSION &&
         data_len != IOT_RF_PACKET_V2_LEN) ||
        (version != IOT_RF_PACKET_V1_VERSION &&
         version != IOT_RF_PACKET_V2_VERSION)) {
        return false;
    }

    if (!iot_rf_packet_command_is_valid(data[3])) {
        return false;
    }

    packet->version = version;
    packet->command = data[3];
    packet->sequence = data[4];
    packet->power_dbm = (int8_t)data[5];
    packet->mode = IOT_RF_MODE_UNKNOWN;

    if (version == IOT_RF_PACKET_V2_VERSION) {
        if (!iot_rf_packet_mode_is_valid(data[6]) || data[7] != 0U) {
            return false;
        }
        packet->mode = data[6];
    }

    return true;
}

bool iot_rf_packet_encode(const iot_rf_packet_t *packet,
                          uint8_t *output,
                          size_t output_size,
                          size_t *encoded_len)
{
    size_t wire_len;

    if (packet == NULL || output == NULL) {
        return false;
    }

    wire_len = iot_rf_packet_wire_length(packet->version);
    if (wire_len == 0U || output_size < wire_len ||
        !iot_rf_packet_command_is_valid(packet->command)) {
        return false;
    }
    if (packet->version == IOT_RF_PACKET_V2_VERSION &&
        !iot_rf_packet_mode_is_valid(packet->mode)) {
        return false;
    }

    output[0] = IOT_RF_PACKET_MAGIC0;
    output[1] = IOT_RF_PACKET_MAGIC1;
    output[2] = packet->version;
    output[3] = packet->command;
    output[4] = packet->sequence;
    output[5] = (uint8_t)packet->power_dbm;

    if (packet->version == IOT_RF_PACKET_V1_VERSION) {
        if (encoded_len != NULL) {
            *encoded_len = wire_len;
        }
        return true;
    }

    output[6] = packet->mode;
    output[7] = 0U;

    if (encoded_len != NULL) {
        *encoded_len = wire_len;
    }
    return true;
}