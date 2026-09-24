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


