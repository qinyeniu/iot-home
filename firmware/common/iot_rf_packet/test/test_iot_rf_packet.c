#include "iot_rf_packet.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_v2_round_trip(void)
{
    static const uint8_t expected[IOT_RF_PACKET_V2_LEN] = {
        0x52, 0x46, 0x02, IOT_RF_CMD_SET, 0x21, 0xf6,
        IOT_RF_MODE_MANUAL, 0x00
    };
    iot_rf_packet_t packet = {
        .version = IOT_RF_PACKET_V2_VERSION,
        .command = IOT_RF_CMD_SET,
        .sequence = 0x21,
        .power_dbm = -10,
        .mode = IOT_RF_MODE_MANUAL,
    };
    uint8_t output[IOT_RF_PACKET_V2_LEN] = {0};
    size_t encoded_len = 0U;

    assert(iot_rf_packet_encode(&packet, output, sizeof(output),
                                &encoded_len));
    assert(encoded_len == IOT_RF_PACKET_V2_LEN);
    assert(memcmp(output, expected, sizeof(output)) == 0);

    memset(&packet, 0, sizeof(packet));
    assert(iot_rf_packet_decode(output, sizeof(output), &packet));
    assert(packet.version == IOT_RF_PACKET_V2_VERSION);
    assert(packet.command == IOT_RF_CMD_SET);
    assert(packet.sequence == 0x21);
    assert(packet.power_dbm == -10);
    assert(packet.mode == IOT_RF_MODE_MANUAL);
}

static void test_v1_round_trip(void)
{
    static const uint8_t expected[IOT_RF_PACKET_V1_LEN] = {
        0x52, 0x46, 0x01, IOT_RF_CMD_REPORT, 0x7f, 0x0e
    };
    iot_rf_packet_t packet = {
        .version = IOT_RF_PACKET_V1_VERSION,
        .command = IOT_RF_CMD_REPORT,
        .sequence = 0x7f,
        .power_dbm = 14,
        /* V1 has no mode byte; this value must be ignored on encode. */
        .mode = IOT_RF_MODE_MANUAL,
    };
    uint8_t output[IOT_RF_PACKET_V2_LEN] = {0};
    size_t encoded_len = 0U;

    assert(iot_rf_packet_wire_length(IOT_RF_PACKET_V1_VERSION) ==
           IOT_RF_PACKET_V1_LEN);
    assert(iot_rf_packet_encode(&packet, output, sizeof(output),
                                &encoded_len));
    assert(encoded_len == IOT_RF_PACKET_V1_LEN);
    assert(memcmp(output, expected, IOT_RF_PACKET_V1_LEN) == 0);

    memset(&packet, 0, sizeof(packet));
    assert(iot_rf_packet_decode(output, IOT_RF_PACKET_V1_LEN, &packet));
    assert(packet.version == IOT_RF_PACKET_V1_VERSION);
    assert(packet.command == IOT_RF_CMD_REPORT);
    assert(packet.sequence == 0x7f);
    assert(packet.power_dbm == 14);
    assert(packet.mode == IOT_RF_MODE_UNKNOWN);
}

static void test_invalid_decodes(void)
{
    uint8_t v2[IOT_RF_PACKET_V2_LEN] = {
        0x52, 0x46, 0x02, IOT_RF_CMD_REPORT, 0x01, 0x08,
        IOT_RF_MODE_AUTO, 0x00
    };
    uint8_t v1[IOT_RF_PACKET_V1_LEN] = {
        0x52, 0x46, 0x01, IOT_RF_CMD_QUERY, 0x02, 0x00
    };
    iot_rf_packet_t packet;

    assert(!iot_rf_packet_decode(NULL, sizeof(v2), &packet));
    assert(!iot_rf_packet_decode(v2, sizeof(v2), NULL));
    assert(!iot_rf_packet_decode(v2, IOT_RF_PACKET_V1_LEN, &packet));
    assert(!iot_rf_packet_decode(v2, 7U, &packet));
    assert(!iot_rf_packet_decode(v2, 9U, &packet));
    assert(!iot_rf_packet_decode(v1, sizeof(v1) - 1U, &packet));
    assert(!iot_rf_packet_decode(v1, IOT_RF_PACKET_V2_LEN, &packet));

    v2[0] = 0x00;
    assert(!iot_rf_packet_decode(v2, sizeof(v2), &packet));
    v2[0] = 0x52;

    v2[2] = 3U;
    assert(!iot_rf_packet_decode(v2, sizeof(v2), &packet));
    v2[2] = IOT_RF_PACKET_V2_VERSION;

    v2[3] = 0U;
    assert(!iot_rf_packet_decode(v2, sizeof(v2), &packet));
    v2[3] = 4U;
    assert(!iot_rf_packet_decode(v2, sizeof(v2), &packet));
    v2[3] = IOT_RF_CMD_REPORT;

    v2[6] = IOT_RF_MODE_UNKNOWN;
    assert(!iot_rf_packet_decode(v2, sizeof(v2), &packet));
    v2[6] = IOT_RF_MODE_AUTO;

    v2[7] = 1U;
    assert(!iot_rf_packet_decode(v2, sizeof(v2), &packet));
    v2[7] = 0U;

    /* A V1 header must not be accepted when carried in an 8-byte buffer. */
    memcpy(v2, v1, sizeof(v1));
    v2[6] = IOT_RF_MODE_AUTO;
    v2[7] = 0U;
    assert(!iot_rf_packet_decode(v2, sizeof(v2), &packet));

    /* A V2 header cannot fit a 6-byte buffer. */
    memcpy(v1, v2, sizeof(v1));
    v1[2] = IOT_RF_PACKET_V2_VERSION;
    assert(!iot_rf_packet_decode(v1, sizeof(v1), &packet));
}

static void test_invalid_encodes(void)
{
    const iot_rf_packet_t packet = {
        .version = IOT_RF_PACKET_V2_VERSION,
        .command = IOT_RF_CMD_QUERY,
        .sequence = 1U,
        .power_dbm = 0,
        .mode = IOT_RF_MODE_AUTO,
    };
    iot_rf_packet_t invalid_packet = packet;
    uint8_t output[IOT_RF_PACKET_V2_LEN];

    assert(iot_rf_packet_wire_length(3U) == 0U);
    assert(!iot_rf_packet_command_is_valid(0U));
    assert(!iot_rf_packet_mode_is_valid(IOT_RF_MODE_UNKNOWN));

    assert(!iot_rf_packet_encode(NULL, output, sizeof(output), NULL));
    assert(!iot_rf_packet_encode(&packet, NULL, sizeof(output), NULL));
    assert(!iot_rf_packet_encode(&packet, output,
                                IOT_RF_PACKET_V2_LEN - 1U, NULL));

    invalid_packet.version = 3U;
    assert(!iot_rf_packet_encode(&invalid_packet, output, sizeof(output),
                                 NULL));
    invalid_packet.version = IOT_RF_PACKET_V2_VERSION;

    invalid_packet.command = 0U;
    assert(!iot_rf_packet_encode(&invalid_packet, output, sizeof(output),
                                 NULL));
    invalid_packet.command = IOT_RF_CMD_QUERY;

    invalid_packet.mode = IOT_RF_MODE_UNKNOWN;
    assert(!iot_rf_packet_encode(&invalid_packet, output, sizeof(output),
                                 NULL));
}

int main(void)
{
    test_v2_round_trip();
    test_v1_round_trip();
    test_invalid_decodes();
    test_invalid_encodes();
    puts("iot_rf_packet host tests passed");
    return 0;
}
