import importlib.util
import sys
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = TOOLS_DIR / "analyze_long_test.py"

spec = importlib.util.spec_from_file_location("analyze_long_test", MODULE_PATH)
analyze_long_test = importlib.util.module_from_spec(spec)
sys.modules["analyze_long_test"] = analyze_long_test
spec.loader.exec_module(analyze_long_test)


def write_log(path: Path, lines):
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def node_stats(uptime: int, rounds: int, aps_fail: int = 0):
    return (
        f"I ({uptime}) zb-sensor: REPORT_STATS: rounds={rounds} repairs=0 "
        f"i2c_th_fail=0 i2c_lux_fail=0 i2c_resets=0 i2c_reinits=0 "
        f"attempts={rounds * 3} queued={rounds * 3} queue_fail=0 "
        f"items_failed=0 aps_ok={rounds * 3 - aps_fail} aps_fail={aps_fail} "
        "unavail=0 recovered=0 candidate=0/0 probe_resteer=0 "
        "probes=0/0 probe_aps_fail=0 th_invalid=0 lux_invalid=0 "
        "th_streak=0 lux_streak=0"
    )


def gateway_line(uptime: int, published: bool):
    if published:
        return (
            f"I ({uptime}) iot-gw: MQTT -> nodes/zb-82cb/telemetry: "
            '{"data":{"temperature":25.0}}'
        )
    return (
        f"I ({uptime}) iot-gw: Zigbee: cached report node=0x82cb "
        "cluster=0x0402 value=2500"
    )


def test_paired_long_test_pass_when_target_duration_met(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(70000, 70)])
    write_log(
        gateway_log,
        [gateway_line(1000, False), gateway_line(70000, True)],
    )

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=60
    )

    assert result["verdict"] == "PASS"
    assert result["duration_s"] == 69.0
    assert result["node"]["deltas"]["rounds"] == 60
    assert result["gateway"]["counts"]["telemetry_published"] == 1
    text = analyze_long_test.format_markdown(result)
    assert "PASS" in text
    assert "rounds" in text


def test_paired_long_test_degraded_on_aps_failure_and_incomplete_by_default(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(70000, 70, aps_fail=2)])
    write_log(gateway_log, [gateway_line(70000, True)])

    result = analyze_long_test.analyze_pair(node_log, gateway_log)

    assert result["verdict"] == "DEGRADED"
    assert result["node"]["deltas"]["aps_fail"] == 2
    assert result["incomplete_reasons"]
    assert any("目标时长" in item for item in result["incomplete_reasons"])


def test_paired_long_test_fail_on_assert_or_reboot(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(
        node_log,
        [
            node_stats(1000, 10),
            "I (2000) zb-sensor: assert failed: lock",
            node_stats(3000, 12),
        ],
    )
    write_log(gateway_log, [gateway_line(3000, True)])

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1
    )

    assert result["verdict"] == "FAIL"
    assert result["failure_reasons"]


def test_paired_long_test_reports_observation_gap(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(200000, 20)])
    write_log(gateway_log, [gateway_line(200000, True)])

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=60,
        gap_threshold_s=120,
    )

    assert result["node"]["observation_gaps"]
    assert result["verdict"] == "DEGRADED"


def test_paired_long_test_incomplete_without_failure_or_degradation(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(70000, 70)])
    write_log(gateway_log, [gateway_line(70000, True)])

    result = analyze_long_test.analyze_pair(node_log, gateway_log)

    assert result["verdict"] == "INCOMPLETE"
    assert result["incomplete_reasons"]
    assert not result["failure_reasons"]


def test_paired_long_test_fail_on_uptime_rollback(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(
        node_log,
        [
            node_stats(1000, 10),
            node_stats(200000, 20),
            node_stats(150000, 15),
        ],
    )
    write_log(gateway_log, [gateway_line(150000, True)])

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1, gap_threshold_s=300
    )

    assert result["verdict"] == "FAIL"
    assert result["node"]["uptime_rollbacks"] == 1
    assert any("uptime" in item for item in result["failure_reasons"])


def test_paired_long_test_fail_on_anis_colored_mac_burst(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10)])
    mac_lines = [
        f"\x1b[0;31mE ({1000 + index * 1000}) ZB_ESP_MAC: No buffer, handle it\x1b[0m"
        for index in range(20)
    ]
    write_log(gateway_log, mac_lines)

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "FAIL"
    assert result["gateway"]["mac_no_buffer"]["total"] == 20
    assert result["gateway"]["mac_no_buffer"]["max_events_in_window"] >= 20
    assert any("MAC" in item for item in result["failure_reasons"])


def test_redacts_common_credentials():
    text = (
        "uri=mqtt://user:pass@host:1883?token=abc "
        'password=wifi-secret token=jwt-token api_key=ak-123'
    )

    safe = analyze_long_test.redact(text)

    assert "user:pass" not in safe
    assert "?token=abc" not in safe
    assert "wifi-secret" not in safe
    assert "jwt-token" not in safe
    assert "ak-123" not in safe
    assert "***" in safe


def test_failure_has_priority_over_degradation(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(
        node_log,
        [
            node_stats(1000, 10),
            "I (2000) zb-sensor: assert failed: lock",
            node_stats(3000, 12, aps_fail=2),
        ],
    )
    write_log(gateway_log, [gateway_line(3000, True)])

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1
    )

    assert result["verdict"] == "FAIL"
    assert result["degraded_reasons"]
    assert result["failure_reasons"]


def test_restart_marker_after_observation_fails(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(
        node_log,
        [node_stats(1000, 10), "rst:0x1 (POWERON_RESET),boot:0x"],
    )
    write_log(gateway_log, [gateway_line(2000, True)])

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1
    )

    assert result["verdict"] == "FAIL"
    assert result["node"]["restart_event_count"] >= 1


def test_single_mac_no_buffer_is_degraded_not_failed(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10)])
    write_log(
        gateway_log,
        [
            "E (1000) ZB_ESP_MAC: No buffer, handle it",
            gateway_line(2000, True),
        ],
    )

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1
    )

    assert result["verdict"] == "DEGRADED"
    assert result["gateway"]["mac_no_buffer"]["total"] == 1


def test_event_samples_are_bounded_with_total_counts(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    lines = [node_stats(1000, 10)]
    lines.extend(
        f"I ({1000 + index}) zb-sensor: assert failed: lock {index}"
        for index in range(25)
    )
    write_log(node_log, lines)
    write_log(gateway_log, [gateway_line(3000, True)])

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1
    )

    assert result["verdict"] == "FAIL"
    assert result["node"]["fatal_event_count"] == 25
    assert len(result["node"]["fatal_events"]) <= 20


def test_empty_logs_are_incomplete(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    node_log.write_text("", encoding="utf-8")
    gateway_log.write_text("", encoding="utf-8")

    result = analyze_long_test.analyze_pair(node_log, gateway_log)

    assert result["verdict"] == "INCOMPLETE"
    assert any("节点日志为空" in item for item in result["incomplete_reasons"])


def gateway_rf_send_line(uptime: int, seq: int, result: str = "ESP_OK"):
    return (
        f"I ({uptime}) iot-gw: RF power -> node=0x82cb cmd=1 power=0 "
        f"mode=auto seq={seq}: {result}"
    )


def test_single_rf_send_failure_is_degraded(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(20000, 20)])
    write_log(
        gateway_log,
        [
            gateway_rf_send_line(1000, 34, "ESP_ERR_NO_MEM"),
            gateway_line(2000, True),
        ],
    )

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1
    )

    assert result["verdict"] == "DEGRADED"
    rf_send = result["gateway"]["rf_send"]
    assert rf_send["failed"] == 1
    assert rf_send["max_consecutive_failures"] == 1
    assert rf_send["status_counts"]["ESP_ERR_NO_MEM"] == 1
    assert any("RF 下发明确失败" in item for item in result["degraded_reasons"])


def test_sustained_rf_send_failure_fails(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(20000, 20)])
    write_log(
        gateway_log,
        [
            gateway_rf_send_line(1000 + index * 1000, 34 + index, "ESP_ERR_NO_MEM")
            for index in range(5)
        ],
    )

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        rf_failure_run_threshold=5,
    )

    assert result["verdict"] == "FAIL"
    rf_send = result["gateway"]["rf_send"]
    assert rf_send["failed"] == 5
    assert rf_send["max_consecutive_failures"] == 5
    assert rf_send["sustained_run_count"] == 1
    assert any("连续" in item for item in result["failure_reasons"])


def test_successful_rf_send_resets_failure_run(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    gateway_lines = [
        gateway_rf_send_line(1000 + index * 1000, 34 + index, "ESP_ERR_NO_MEM")
        for index in range(4)
    ]
    gateway_lines.append(gateway_rf_send_line(5000, 38, "ESP_OK"))
    gateway_lines.extend(
        gateway_rf_send_line(6000 + index * 1000, 39 + index, "ESP_ERR_NO_MEM")
        for index in range(4)
    )
    write_log(node_log, [node_stats(1000, 10), node_stats(20000, 20)])
    write_log(gateway_log, gateway_lines)

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        rf_failure_run_threshold=5,
    )

    assert result["verdict"] == "DEGRADED"
    rf_send = result["gateway"]["rf_send"]
    assert rf_send["failed"] == 8
    assert rf_send["ok"] == 1
    assert rf_send["max_consecutive_failures"] == 4
    assert rf_send["sustained_run_count"] == 0


def test_rf_failure_run_resets_after_restart_marker(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(10000, 20)])
    write_log(
        gateway_log,
        [
            gateway_rf_send_line(1000, 1, "ESP_ERR_NO_MEM"),
            gateway_rf_send_line(2000, 2, "ESP_ERR_NO_MEM"),
            "rst:0x1 (POWERON_RESET),boot:0x",
            gateway_rf_send_line(1000, 3, "ESP_ERR_NO_MEM"),
        ],
    )

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        rf_failure_run_threshold=5,
    )

    assert result["verdict"] == "FAIL"
    rf_send = result["gateway"]["rf_send"]
    assert rf_send["failed"] == 3
    assert rf_send["max_consecutive_failures"] == 2
    assert rf_send["status_counts"]["ESP_ERR_NO_MEM"] == 3


def test_unknown_rf_send_result_is_degraded_but_not_failed(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(10000, 20)])
    write_log(
        gateway_log,
        [
            "I (1000) iot-gw: RF power -> node=0x82cb cmd=1 power=0 "
            "mode=auto seq=34",
            gateway_line(2000, True),
        ],
    )

    result = analyze_long_test.analyze_pair(
        node_log, gateway_log, target_duration_s=1
    )

    assert result["verdict"] == "DEGRADED"
    rf_send = result["gateway"]["rf_send"]
    assert rf_send["failed"] == 0
    assert rf_send["unknown"] == 1
    assert rf_send["max_consecutive_failures"] == 0
    assert any("无法解析" in item for item in result["degraded_reasons"])


def test_rf_failure_samples_are_bounded_but_counts_remain_total(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(node_log, [node_stats(1000, 10), node_stats(30000, 20)])
    write_log(
        gateway_log,
        [
            gateway_rf_send_line(1000 + index * 1000, 1 + index, "ESP_ERR_NO_MEM")
            for index in range(25)
        ],
    )

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        rf_failure_run_threshold=5,
    )

    rf_send = result["gateway"]["rf_send"]
    assert result["verdict"] == "FAIL"
    assert rf_send["failure_count"] == 25
    assert rf_send["failed"] == 25
    assert len(rf_send["failures"]) <= 20
    assert rf_send["max_consecutive_failures"] == 25


def test_second_sustained_rf_failure_run_is_counted_after_recovery(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    gateway_lines = [
        gateway_rf_send_line(1000 + index * 1000, 1 + index, "ESP_ERR_NO_MEM")
        for index in range(5)
    ]
    gateway_lines.append(gateway_rf_send_line(6000, 6, "ESP_OK"))
    gateway_lines.extend(
        gateway_rf_send_line(7000 + index * 1000, 7 + index, "ESP_ERR_NO_MEM")
        for index in range(5)
    )
    write_log(node_log, [node_stats(1000, 10), node_stats(20000, 20)])
    write_log(gateway_log, gateway_lines)

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        rf_failure_run_threshold=5,
    )

    rf_send = result["gateway"]["rf_send"]
    assert result["verdict"] == "FAIL"
    assert rf_send["failed"] == 10
    assert rf_send["ok"] == 1
    assert rf_send["max_consecutive_failures"] == 5
    assert rf_send["sustained_run_count"] == 2


def test_redacts_json_quoted_credentials():
    text = '{"password":"secret","api-key":"abc"}'

    safe = analyze_long_test.redact(text)

    assert "secret" not in safe
    assert "abc" not in safe
    assert "***" in safe


def test_fail_on_rejoin_and_wifi_storm_during_business_blackout(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    node_lines = [
        node_stats(1000, 10),
        "I (1000) zb-sensor: DATA: {'temp':25.0}",
        "I (1000) zb-sensor: REPORT_SEND: reason=sample flags=0x07",
        "W (305000) zb-sensor: Zigbee: Startup/rejoin failed",
    ]
    node_lines.extend(
        "ZBOSS: nwk/nwk_join.c:629   Rejoin failure: no dev with xpanid"
        for _ in range(25)
    )
    node_lines.append("I (310000) zb-sensor: Waiting for Zigbee network...")
    gateway_lines = [
        gateway_line(1000, True),
    ]
    gateway_lines.extend(
        f"W ({305000 + index * 100}) wifi:m f null"
        for index in range(25)
    )
    write_log(node_log, node_lines)
    write_log(gateway_log, gateway_lines)

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "FAIL"
    assert result["paired_business_blackouts"]
    assert result["node"]["node_rejoin_storm"]["significant_burst_count"] == 1
    assert result["gateway"]["wifi_mf_storm"]["significant_burst_count"] == 1
    assert any("rejoin failure" in item for item in result["failure_reasons"])
    assert any("m f null" in item for item in result["failure_reasons"])
    report = analyze_long_test.format_markdown(result)
    assert "业务连续性" in report
    assert "错误风暴" in report


def test_plain_business_blackout_is_degraded_not_failed(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    write_log(
        node_log,
        [
            node_stats(1000, 10),
            "I (1000) zb-sensor: DATA: {'temp':25.0}",
            "I (1000) zb-sensor: REPORT_SEND: reason=sample flags=0x07",
            "I (310000) zb-sensor: Waiting for Zigbee network...",
        ],
    )
    write_log(
        gateway_log,
        [gateway_line(1000, True), gateway_line(310000, False)],
    )

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "DEGRADED"
    assert result["paired_business_blackouts"]
    assert not result["failure_reasons"]


def test_storm_without_business_blackout_is_degraded(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    node_lines = [
        node_stats(1000, 10),
        "I (1000) zb-sensor: DATA: {'temp':25.0}",
        "I (1000) zb-sensor: REPORT_SEND: reason=sample flags=0x07",
        "W (5000) zb-sensor: Zigbee: Startup/rejoin failed",
    ]
    node_lines.extend(
        "ZBOSS: nwk/nwk_join.c:629   Rejoin failure: no dev with xpanid"
        for _ in range(25)
    )
    node_lines.extend(
        [
            "I (10000) zb-sensor: DATA: {'temp':25.1}",
            "I (10000) zb-sensor: REPORT_SEND: reason=sample flags=0x07",
            node_stats(10000, 11),
        ]
    )
    gateway_lines = [
        gateway_line(1000, True),
    ]
    gateway_lines.extend(
        f"W ({5000 + index * 100}) wifi:m f null"
        for index in range(25)
    )
    gateway_lines.append(gateway_line(10000, True))
    write_log(node_log, node_lines)
    write_log(gateway_log, gateway_lines)

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "DEGRADED"
    assert result["node"]["node_rejoin_storm"]["significant_burst_count"] == 1
    assert result["gateway"]["wifi_mf_storm"]["significant_burst_count"] == 1


def test_single_node_rejoin_outage_is_degraded_not_failed(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    node_lines = [
        node_stats(1000, 10),
        "I (1000) zb-sensor: DATA: {'temp':25.0}",
        "I (1000) zb-sensor: REPORT_SEND: reason=sample",
        "W (305000) zb-sensor: Startup/rejoin failed",
    ]
    node_lines.extend(
        "ZBOSS: nwk/nwk_join.c:629   Rejoin failure: no dev"
        for _ in range(25)
    )
    write_log(node_log, node_lines)
    write_log(gateway_log, [gateway_line(305000, True)])

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "DEGRADED"
    assert not result["paired_business_blackouts"]
    assert any("节点上报出现中断" in r for r in result["degraded_reasons"])
    assert not result["failure_reasons"]


def test_single_gateway_wifi_outage_is_degraded_not_failed(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    gateway_lines = [
        gateway_line(1000, True),
    ]
    gateway_lines.extend(
        f"W ({305000 + index * 100}) wifi:m f null"
        for index in range(25)
    )
    node_lines = []
    for uptime, rounds in ((1000, 10), (100000, 10), (310000, 11)):
        node_lines.extend(
            [
                node_stats(uptime, rounds),
                f"I ({uptime}) zb-sensor: DATA: {{'temp':25.0}}",
                f"I ({uptime}) zb-sensor: REPORT_SEND: reason=sample",
            ]
        )
    write_log(node_log, node_lines)
    write_log(gateway_log, gateway_lines)

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "DEGRADED"
    assert not result["paired_business_blackouts"]
    assert any("网关 MQTT 出现中断" in r for r in result["degraded_reasons"])
    assert not result["failure_reasons"]


def test_raw_rejoin_tail_is_degraded_but_not_timed_failure(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"
    node_lines = [
        node_stats(1000, 10),
        "I (1000) zb-sensor: DATA: {'temp':25.0}",
        "I (1000) zb-sensor: REPORT_SEND: reason=sample",
    ]
    node_lines.extend(
        "ZBOSS: nwk/nwk_join.c:629   Rejoin failure: no dev"
        for _ in range(25)
    )
    write_log(node_log, node_lines)
    write_log(gateway_log, [gateway_line(1000, True)])

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=1,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "DEGRADED"
    assert result["node"]["counts"]["raw_rejoin_failures"] == 25
    assert any("无时间戳" in r for r in result["degraded_reasons"])
    assert not result["failure_reasons"]


def test_uptime_rollback_resets_segment_timeline(tmp_path):
    node_log = tmp_path / "node.txt"
    gateway_log = tmp_path / "gateway.txt"

    def node_business(uptime: int, rounds: int):
        return [
            node_stats(uptime, rounds),
            f"I ({uptime}) zb-sensor: DATA: {{'temp':25.0}}",
            f"I ({uptime}) zb-sensor: REPORT_SEND: reason=sample",
        ]

    node_lines = node_business(1000, 10)
    node_lines.extend(node_business(200000, 20))
    node_lines.extend(node_business(1000, 1))
    node_lines.extend(node_business(50000, 5))

    gateway_lines = [gateway_line(1000, True), gateway_line(200000, True)]
    gateway_lines.extend(
        [gateway_line(1000, True), gateway_line(50000, True)]
    )

    write_log(node_log, node_lines)
    write_log(gateway_log, gateway_lines)

    result = analyze_long_test.analyze_pair(
        node_log,
        gateway_log,
        target_duration_s=300,
        gap_threshold_s=300,
    )

    assert result["verdict"] == "FAIL"
    assert result["node"]["uptime_rollbacks"] == 1
    assert result["duration_s"] == 248.0
    assert len(result["node"]["boot_segments"]) == 2
    assert len(result["gateway"]["boot_segments"]) == 2
    assert not result["paired_business_blackouts"]


def test_timed_storm_uses_half_open_window_boundary():
    storm = analyze_long_test.TimedEventStorm(
        "test", window_s=60, threshold=2
    )

    storm.add(0, 1)
    storm.add(60000, 2)

    assert storm.total == 2
    assert storm.max_events_in_window == 1
    assert storm.significant_burst_count == 0
