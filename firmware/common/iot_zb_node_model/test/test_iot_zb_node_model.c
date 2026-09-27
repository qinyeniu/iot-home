#include "iot_zb_node_model.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

static void make_ieee(uint8_t ieee[8], uint8_t seed)
{
    for (uint8_t i = 0; i < 8; ++i) {
        ieee[i] = (uint8_t)(seed + i);
    }
}

static void test_store_lookup_and_capacity(void)
{
    iot_zb_node_t nodes[3] = {0};
    iot_zb_node_store_t store = { .nodes = nodes, .capacity = 3 };
    uint8_t ieee[8];

    assert(iot_zb_node_store_valid(&store));
    assert(!iot_zb_node_store_valid(NULL));

    make_ieee(ieee, 0x10);
    assert(iot_zb_node_find(&store, 0x1234) == IOT_ZB_NODE_INVALID);
    assert(iot_zb_node_get_or_create(&store, 0x1234) == 0);
    assert(iot_zb_node_get_or_create(&store, 0x1234) == 0);
    assert(iot_zb_node_get_or_create(&store, 0x2345) == 1);
    assert(iot_zb_node_get_or_create(&store, 0x3456) == 2);
    assert(iot_zb_node_get_or_create(&store, 0x4567) == IOT_ZB_NODE_INVALID);

    uint8_t unknown_ieee[8];

    memcpy(nodes[1].ieee, ieee, sizeof(ieee));
    make_ieee(unknown_ieee, 0xee);
    assert(iot_zb_node_find_ieee(&store, ieee) == 1);
    assert(iot_zb_node_find_ieee(&store, unknown_ieee) == IOT_ZB_NODE_INVALID);

    assert(!iot_zb_node_is_active(&store, 0x1234));
    nodes[0].active = true;
    assert(iot_zb_node_is_active(&store, 0x1234));
}

static void test_join_and_leave(void)
{
    iot_zb_node_t nodes[2] = {0};
    iot_zb_node_store_t store = { .nodes = nodes, .capacity = 2 };
    iot_zb_node_result_t result;
    uint8_t ieee[8];

    make_ieee(ieee, 0x20);
    assert(iot_zb_node_apply_join(&store, 0x82cb, ieee, 100, &result));
    assert(result.found && result.became_active && result.index == 0);
    assert(nodes[0].active && nodes[0].last_seen_ms == 100);
    assert(memcmp(nodes[0].ieee, ieee, 8) == 0);

    assert(iot_zb_node_apply_join(&store, 0x82cb, ieee, 200, &result));
    assert(result.found && !result.became_active);
    assert(nodes[0].last_seen_ms == 200);

    nodes[0].dirty = IOT_ZB_MASK_TEMP;
    assert(iot_zb_node_apply_leave(&store, ieee, &result));
    assert(result.found && result.was_active);
    assert(!nodes[0].active && nodes[0].dirty == 0);

    assert(iot_zb_node_apply_leave(&store, ieee, &result));
    assert(result.found && !result.was_active);

    make_ieee(ieee, 0x90);
    assert(iot_zb_node_apply_leave(&store, ieee, &result));
    assert(!result.found && result.index == IOT_ZB_NODE_INVALID);
}

static void test_measurement_and_onoff_reports(void)
{
    iot_zb_node_t nodes[2] = {0};
    iot_zb_node_store_t store = { .nodes = nodes, .capacity = 2 };
    iot_zb_node_result_t result;
    uint32_t last_seen = 0;

    assert(iot_zb_node_apply_measurement(
        &store, 0x82cb, IOT_ZB_MASK_TEMP, 2500, 100, &result));
    assert(result.found && result.created == false);
    assert(result.became_active && result.first_dirty && result.updated);
    assert(nodes[0].active && nodes[0].temp_raw == 2500);
    assert(nodes[0].valid == IOT_ZB_MASK_TEMP);
    assert(nodes[0].dirty == IOT_ZB_MASK_TEMP);
    assert(nodes[0].first_dirty_ms == 100);

    assert(iot_zb_node_apply_measurement(
        &store, 0x82cb, IOT_ZB_MASK_HUM, 5520, 200, &result));
    assert(!result.became_active && !result.first_dirty);
    assert(nodes[0].hum_raw == 5520);
    assert(nodes[0].first_dirty_ms == 100);

    assert(iot_zb_node_apply_measurement(
        &store, 0x82cb, IOT_ZB_MASK_LUX, 10000, 300, &result));
    assert(nodes[0].lux_raw == 10000);

    assert(iot_zb_node_get_last_seen(&store, 0x82cb, &last_seen));
    assert(last_seen == 300);

    assert(!iot_zb_node_apply_measurement(
        &store, 0x82cb, IOT_ZB_MASK_TEMP, (uint16_t)IOT_ZB_TEMP_INVALID,
        400, &result));
    assert(!iot_zb_node_apply_measurement(
        &store, 0x82cb, IOT_ZB_MASK_HUM, IOT_ZB_VALUE_INVALID, 400, &result));
    assert(!iot_zb_node_apply_measurement(
        &store, 0x82cb, IOT_ZB_MASK_TEMP | IOT_ZB_MASK_HUM, 1, 400, &result));
    assert(!iot_zb_node_apply_measurement(&store, 0x82cb, 0, 1, 400, &result));

    /* Invalid inputs for an unused address must not allocate its slot. */
    assert(!iot_zb_node_apply_measurement(
        &store, 0x1234, IOT_ZB_MASK_TEMP, (uint16_t)IOT_ZB_TEMP_INVALID,
        400, &result));
    assert(iot_zb_node_find(&store, 0x1234) == IOT_ZB_NODE_INVALID);
    assert(!iot_zb_node_apply_onoff(&store, 0x1234, 2, 400, &result));
    assert(iot_zb_node_find(&store, 0x1234) == IOT_ZB_NODE_INVALID);
    assert(!iot_zb_node_apply_join(
        &store, 0x1234, NULL, 400, &result));
    assert(iot_zb_node_find(&store, 0x1234) == IOT_ZB_NODE_INVALID);

    assert(iot_zb_node_apply_onoff(&store, 0x82cb, 1, 500, &result));
    assert(nodes[0].onoff_valid && nodes[0].onoff_value == 1);
    assert(!iot_zb_node_apply_onoff(&store, 0x82cb, 2, 600, &result));
}

static void test_take_due_and_snapshots(void)
{
    iot_zb_node_t nodes[3] = {0};
    iot_zb_node_store_t store = { .nodes = nodes, .capacity = 3 };
    iot_zb_node_result_t result;
    iot_zb_dirty_node_t due[3];
    iot_zb_node_t active[3];

    assert(iot_zb_node_apply_measurement(
        &store, 1, IOT_ZB_MASK_TEMP, 2500, 1000, &result));

    assert(iot_zb_node_take_due(&store, 30999, 30000, due, 3) == 0);
    assert(iot_zb_node_take_due(&store, 31000, 30000, due, 3) == 1);
    assert(due[0].index == 0 && due[0].node.temp_raw == 2500);
    assert(nodes[0].dirty == 0);

    assert(iot_zb_node_apply_measurement(
        &store, 2, IOT_ZB_MASK_TEMP, 2600, 1000, &result));
    assert(iot_zb_node_apply_measurement(
        &store, 2, IOT_ZB_MASK_HUM, 5500, 1000, &result));
    assert(iot_zb_node_apply_measurement(
        &store, 2, IOT_ZB_MASK_LUX, 10000, 1000, &result));
    assert(nodes[1].dirty == IOT_ZB_MASK_ALL);

    assert(iot_zb_node_take_due(&store, 31000, 30000, due, 3) == 1);
    assert(due[0].index == 1);
    assert(nodes[1].dirty == 0);

    assert(iot_zb_node_count_active(&store) == 2);
    assert(iot_zb_node_snapshot_active(&store, active, 3) == 2);
    assert(active[0].addr == 1 && active[1].addr == 2);
}

static void test_telemetry_builder(void)
{
    iot_zb_node_t node = {0};
    char buffer[160];
    char small[16];

    node.addr = 0x82cb;
    node.active = true;
    node.temp_raw = 2500;
    node.hum_raw = 5520;
    node.lux_raw = 10000;
    node.valid = IOT_ZB_MASK_ALL;
    node.onoff_valid = true;
    node.onoff_value = 1;

    assert(iot_zb_node_build_telemetry(&node, buffer, sizeof(buffer)));
    assert(strcmp(buffer,
        "{\"data\":{\"temperature\":25.0,\"humidity\":55.2,"
        "\"lux\":9.0,\"on_off\":1}}") == 0);

    assert(!iot_zb_node_build_telemetry(&node, small, sizeof(small)));
}

int main(void)
{
    test_store_lookup_and_capacity();
    test_join_and_leave();
    test_measurement_and_onoff_reports();
    test_take_due_and_snapshots();
    test_telemetry_builder();
    puts("iot_zb_node_model host tests passed");
    return 0;
}