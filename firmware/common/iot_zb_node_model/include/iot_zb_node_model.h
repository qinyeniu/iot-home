#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_ZB_MASK_TEMP              0x01
#define IOT_ZB_MASK_HUM               0x02
#define IOT_ZB_MASK_LUX               0x04
#define IOT_ZB_MASK_ALL \
    (IOT_ZB_MASK_TEMP | IOT_ZB_MASK_HUM | IOT_ZB_MASK_LUX)

#define IOT_ZB_TEMP_INVALID           ((int16_t)0x8000)
#define IOT_ZB_VALUE_INVALID          0xFFFF
#define IOT_ZB_NODE_INVALID           0xFFu

/** A single Zigbee child node and the latest cached application state. */
typedef struct {
    uint16_t addr;
    int16_t temp_raw;
    uint16_t hum_raw;
    uint16_t lux_raw;
    uint8_t ieee[8];
    uint8_t valid;
    uint8_t dirty;
    uint8_t active;
    uint8_t onoff_valid;
    uint8_t onoff_value;
    uint32_t first_dirty_ms;
    uint32_t last_seen_ms;
} iot_zb_node_t;

/** Caller-owned backing array. The model does not allocate memory. */
typedef struct {
    iot_zb_node_t *nodes;
    uint8_t capacity;
} iot_zb_node_store_t;

/** Details from one model mutation. Not all fields apply to every operation. */
typedef struct {
    uint8_t index;
    bool found;
    bool created;
    bool became_active;
    bool was_active;
    bool first_dirty;
    bool updated;
} iot_zb_node_result_t;

/** A copied dirty node plus its slot in the caller-owned store. */
typedef struct {
    uint8_t index;
    iot_zb_node_t node;
} iot_zb_dirty_node_t;

bool iot_zb_node_store_valid(const iot_zb_node_store_t *store);

uint8_t iot_zb_node_find(const iot_zb_node_store_t *store, uint16_t addr);
uint8_t iot_zb_node_find_ieee(const iot_zb_node_store_t *store,
                              const uint8_t ieee[8]);
uint8_t iot_zb_node_get_or_create(const iot_zb_node_store_t *store,
                                  uint16_t addr);

bool iot_zb_node_is_active(const iot_zb_node_store_t *store, uint16_t addr);
bool iot_zb_node_get_last_seen(const iot_zb_node_store_t *store,
                               uint16_t addr, uint32_t *last_seen_ms);
uint8_t iot_zb_node_count_active(const iot_zb_node_store_t *store);
uint8_t iot_zb_node_snapshot_active(const iot_zb_node_store_t *store,
                                    iot_zb_node_t *out, uint8_t max_count);

bool iot_zb_node_apply_join(const iot_zb_node_store_t *store, uint16_t addr,
                            const uint8_t ieee[8], uint32_t now_ms,
                            iot_zb_node_result_t *result);
bool iot_zb_node_apply_leave(const iot_zb_node_store_t *store,
                             const uint8_t ieee[8],
                             iot_zb_node_result_t *result);
bool iot_zb_node_apply_measurement(const iot_zb_node_store_t *store,
                                   uint16_t addr, uint8_t field_mask,
                                   uint16_t raw, uint32_t now_ms,
                                   iot_zb_node_result_t *result);
bool iot_zb_node_apply_onoff(const iot_zb_node_store_t *store, uint16_t addr,
                             uint8_t value, uint32_t now_ms,
                             iot_zb_node_result_t *result);

uint8_t iot_zb_node_take_due(const iot_zb_node_store_t *store,
                             uint32_t now_ms, uint32_t partial_delay_ms,
                             iot_zb_dirty_node_t *out, uint8_t max_count);

/** Build the telemetry payload. Returns false if output is too small. */
bool iot_zb_node_build_telemetry(const iot_zb_node_t *node,
                                 char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif