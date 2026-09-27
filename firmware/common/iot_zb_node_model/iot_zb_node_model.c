#include "iot_zb_node_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void result_init(iot_zb_node_result_t *result, uint8_t index)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->index = index;
}

bool iot_zb_node_store_valid(const iot_zb_node_store_t *store)
{
    return store != NULL && store->nodes != NULL && store->capacity != 0U;
}

uint8_t iot_zb_node_find(const iot_zb_node_store_t *store, uint16_t addr)
{
    uint8_t i;

    if (!iot_zb_node_store_valid(store) || addr == 0U) {
        return IOT_ZB_NODE_INVALID;
    }

    for (i = 0; i < store->capacity; ++i) {
        if (store->nodes[i].addr == addr) {
            return i;
        }
    }
    return IOT_ZB_NODE_INVALID;
}

uint8_t iot_zb_node_find_ieee(const iot_zb_node_store_t *store,
                              const uint8_t ieee[8])
{
    uint8_t i;

    if (!iot_zb_node_store_valid(store) || ieee == NULL) {
        return IOT_ZB_NODE_INVALID;
    }

    for (i = 0; i < store->capacity; ++i) {
        if (store->nodes[i].addr != 0U &&
            memcmp(store->nodes[i].ieee, ieee, sizeof(store->nodes[i].ieee)) == 0) {
            return i;
        }
    }
    return IOT_ZB_NODE_INVALID;
}

uint8_t iot_zb_node_get_or_create(const iot_zb_node_store_t *store,
                                  uint16_t addr)
{
    iot_zb_node_t *node = NULL;
    uint8_t i;

    if (!iot_zb_node_store_valid(store) || addr == 0U) {
        return IOT_ZB_NODE_INVALID;
    }

    for (i = 0; i < store->capacity; ++i) {
        if (store->nodes[i].addr == addr) {
            return i;
        }
        if (store->nodes[i].addr == 0U && node == NULL) {
            node = &store->nodes[i];
        }
    }

    if (node != NULL) {
        memset(node, 0, sizeof(*node));
        node->addr = addr;
        return (uint8_t)(node - store->nodes);
    }
    return IOT_ZB_NODE_INVALID;
}

bool iot_zb_node_is_active(const iot_zb_node_store_t *store, uint16_t addr)
{
    uint8_t index = iot_zb_node_find(store, addr);
    return index != IOT_ZB_NODE_INVALID && store->nodes[index].active;
}

bool iot_zb_node_get_last_seen(const iot_zb_node_store_t *store,
                               uint16_t addr, uint32_t *last_seen_ms)
{
    uint8_t index;

    if (last_seen_ms != NULL) {
        *last_seen_ms = 0U;
    }
    index = iot_zb_node_find(store, addr);
    if (index == IOT_ZB_NODE_INVALID || !store->nodes[index].active ||
        last_seen_ms == NULL) {
        return false;
    }
    *last_seen_ms = store->nodes[index].last_seen_ms;
    return true;
}

uint8_t iot_zb_node_count_active(const iot_zb_node_store_t *store)
{
    uint8_t count = 0;
    uint8_t i;

    if (!iot_zb_node_store_valid(store)) {
        return 0;
    }
    for (i = 0; i < store->capacity; ++i) {
        if (store->nodes[i].addr != 0U && store->nodes[i].active) {
            ++count;
        }
    }
    return count;
}

uint8_t iot_zb_node_snapshot_active(const iot_zb_node_store_t *store,
                                    iot_zb_node_t *out, uint8_t max_count)
{
    uint8_t count = 0;
    uint8_t i;

    if (!iot_zb_node_store_valid(store) || out == NULL) {
        return 0;
    }
    for (i = 0; i < store->capacity; ++i) {
        if (store->nodes[i].addr != 0U && store->nodes[i].active) {
            if (count >= max_count) {
                break;
            }
            out[count++] = store->nodes[i];
        }
    }
    return count;
}

bool iot_zb_node_apply_join(const iot_zb_node_store_t *store, uint16_t addr,
                            const uint8_t ieee[8], uint32_t now_ms,
                            iot_zb_node_result_t *result)
{
    uint8_t index;
    iot_zb_node_t *node;

    if (ieee == NULL) {
        result_init(result, IOT_ZB_NODE_INVALID);
        return false;
    }

    index = iot_zb_node_get_or_create(store, addr);
    if (index == IOT_ZB_NODE_INVALID) {
        result_init(result, IOT_ZB_NODE_INVALID);
        return false;
    }

    node = &store->nodes[index];
    result_init(result, index);
    result->found = true;
    result->created = (node->valid == 0U && node->dirty == 0U &&
                       !node->active && node->last_seen_ms == 0U);

    if (!node->active) {
        node->active = true;
        result->became_active = true;
    }
    memcpy(node->ieee, ieee, sizeof(node->ieee));
    node->last_seen_ms = now_ms;
    result->updated = true;
    return true;
}

bool iot_zb_node_apply_leave(const iot_zb_node_store_t *store,
                             const uint8_t ieee[8],
                             iot_zb_node_result_t *result)
{
    uint8_t index;
    iot_zb_node_t *node;

    if (!iot_zb_node_store_valid(store) || ieee == NULL) {
        result_init(result, IOT_ZB_NODE_INVALID);
        return false;
    }

    index = iot_zb_node_find_ieee(store, ieee);
    if (index == IOT_ZB_NODE_INVALID) {
        result_init(result, IOT_ZB_NODE_INVALID);
        return true;
    }

    node = &store->nodes[index];
    result_init(result, index);
    result->found = true;
    result->was_active = node->active;
    if (node->active) {
        node->active = false;
        node->dirty = 0U;
    }
    return true;
}

bool iot_zb_node_apply_measurement(const iot_zb_node_store_t *store,
                                   uint16_t addr, uint8_t field_mask,
                                   uint16_t raw, uint32_t now_ms,
                                   iot_zb_node_result_t *result)
{
    uint8_t index;
    iot_zb_node_t *node;

    result_init(result, IOT_ZB_NODE_INVALID);
    if ((field_mask & IOT_ZB_MASK_ALL) == 0U ||
        (field_mask & ~IOT_ZB_MASK_ALL) != 0U ||
        (field_mask & (field_mask - 1U)) != 0U) {
        return false;
    }
    if (((field_mask == IOT_ZB_MASK_TEMP && raw == (uint16_t)IOT_ZB_TEMP_INVALID) ||
         (field_mask != IOT_ZB_MASK_TEMP && raw == IOT_ZB_VALUE_INVALID))) {
        return false;
    }

    index = iot_zb_node_get_or_create(store, addr);
    if (index == IOT_ZB_NODE_INVALID) {
        return false;
    }

    node = &store->nodes[index];
    result_init(result, index);
    result->found = true;

    if (!node->active) {
        node->active = true;
        result->became_active = true;
    }
    result->first_dirty = (node->dirty == 0U);

    if (field_mask == IOT_ZB_MASK_TEMP) {
        node->temp_raw = (int16_t)raw;
    } else if (field_mask == IOT_ZB_MASK_HUM) {
        node->hum_raw = raw;
    } else {
        node->lux_raw = raw;
    }
    node->valid |= field_mask;
    node->dirty |= field_mask;
    if (result->first_dirty) {
        node->first_dirty_ms = now_ms;
    }
    node->last_seen_ms = now_ms;
    result->updated = true;
    return true;
}

bool iot_zb_node_apply_onoff(const iot_zb_node_store_t *store, uint16_t addr,
                             uint8_t value, uint32_t now_ms,
                             iot_zb_node_result_t *result)
{
    uint8_t index;
    iot_zb_node_t *node;

    result_init(result, IOT_ZB_NODE_INVALID);
    if (value > 1U) {
        return false;
    }

    index = iot_zb_node_get_or_create(store, addr);
    if (index == IOT_ZB_NODE_INVALID) {
        return false;
    }

    node = &store->nodes[index];
    result_init(result, index);
    result->found = true;
    if (!node->active) {
        node->active = true;
        result->became_active = true;
    }
    result->first_dirty = (node->dirty == 0U);
    node->onoff_value = value;
    node->onoff_valid = true;
    if (result->first_dirty) {
        node->first_dirty_ms = now_ms;
    }
    node->last_seen_ms = now_ms;
    result->updated = true;
    return true;
}

uint8_t iot_zb_node_take_due(const iot_zb_node_store_t *store,
                             uint32_t now_ms, uint32_t partial_delay_ms,
                             iot_zb_dirty_node_t *out, uint8_t max_count)
{
    uint8_t count = 0;
    uint8_t i;

    if (!iot_zb_node_store_valid(store) || out == NULL) {
        return 0;
    }

    for (i = 0; i < store->capacity; ++i) {
        iot_zb_node_t *node = &store->nodes[i];
        uint32_t age_ms;
        bool complete;

        if (node->addr == 0U || !node->active || node->dirty == 0U ||
            count >= max_count) {
            continue;
        }

        age_ms = now_ms - node->first_dirty_ms;
        complete = (node->dirty & IOT_ZB_MASK_ALL) == IOT_ZB_MASK_ALL;
        if (!complete && age_ms < partial_delay_ms) {
            continue;
        }

        out[count].index = i;
        out[count].node = *node;
        node->dirty = 0U;
        ++count;
    }
    return count;
}

bool iot_zb_node_build_telemetry(const iot_zb_node_t *node,
                                 char *buffer, size_t buffer_size)
{
    size_t pos = 0;
    bool field_written = false;

    if (node == NULL || buffer == NULL || buffer_size == 0U) {
        return false;
    }

#define APPEND(...)                                                         \
    do {                                                                    \
        int written = snprintf(buffer + pos, buffer_size - pos, __VA_ARGS__);\
        if (written < 0 || (size_t)written >= buffer_size - pos) {           \
            return false;                                                   \
        }                                                                   \
        pos += (size_t)written;                                             \
    } while (0)

    APPEND("{\"data\":{");
    if (node->valid & IOT_ZB_MASK_TEMP) {
        APPEND("\"temperature\":%.1f,", node->temp_raw / 100.0f);
        field_written = true;
    }
    if (node->valid & IOT_ZB_MASK_HUM) {
        APPEND("\"humidity\":%.1f,", node->hum_raw / 100.0f);
        field_written = true;
    }
    if (node->valid & IOT_ZB_MASK_LUX) {
        float lux = powf(10.0f, (float)node->lux_raw / 10000.0f) - 1.0f;
        APPEND("\"lux\":%.1f,", lux);
        field_written = true;
    }
    if (node->onoff_valid) {
        APPEND("\"on_off\":%u,", (unsigned)node->onoff_value);
        field_written = true;
    }
    if (pos > 0U && buffer[pos - 1] == ',') {
        --pos;
        buffer[pos] = '\0';
    }
    APPEND("}}");
#undef APPEND
    return field_written;
}