#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_RF_PACKET_MAGIC0 0x52U
#define IOT_RF_PACKET_MAGIC1 0x46U

#define IOT_RF_PACKET_V1_VERSION 1U
#define IOT_RF_PACKET_V2_VERSION 2U

#define IOT_RF_PACKET_V1_LEN 6U
#define IOT_RF_PACKET_V2_LEN 8U

#define IOT_RF_CMD_QUERY  1U
#define IOT_RF_CMD_SET    2U
#define IOT_RF_CMD_REPORT 3U

#define IOT_RF_MODE_AUTO   0U
#define IOT_RF_MODE_MANUAL 1U
#define IOT_RF_MODE_UNKNOWN 0xFFU

/** Decoded private RF power packet. */
typedef struct {
    uint8_t version;
    uint8_t command;
    uint8_t sequence;
    int8_t power_dbm;

    /** IOT_RF_MODE_AUTO/MANUAL for V2; IOT_RF_MODE_UNKNOWN for V1. */
    uint8_t mode;
} iot_rf_packet_t;

/** Return the wire length for a supported packet version; 0 if unsupported. */
size_t iot_rf_packet_wire_length(uint8_t version);

/** Return true for QUERY, SET or REPORT. */
bool iot_rf_packet_command_is_valid(uint8_t command);

/** Return true for AUTO or MANUAL. */
bool iot_rf_packet_mode_is_valid(uint8_t mode);

/**
 * Validate and decode one complete private RF packet.
 *
 * V1 has no mode byte, so mode is set to IOT_RF_MODE_UNKNOWN.
 * V2 additionally requires a valid AUTO/MANUAL mode and a zero reserved byte.
 */
bool iot_rf_packet_decode(const uint8_t *data,
                          size_t data_len,
                          iot_rf_packet_t *packet);

/**
 * Encode a packet described by the caller.
 *
 * For V2, mode must be AUTO or MANUAL. For V1, mode is ignored. The output
 * length is written through encoded_len when the pointer is non-NULL.
 */
bool iot_rf_packet_encode(const iot_rf_packet_t *packet,
                          uint8_t *output,
                          size_t output_size,
                          size_t *encoded_len);

#ifdef __cplusplus
}
#endif