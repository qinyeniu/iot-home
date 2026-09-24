import importlib.util
import sys
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = TOOLS_DIR / "analyze_serial_log.py"

spec = importlib.util.spec_from_file_location("analyze_serial_log", MODULE_PATH)
analyze_serial_log = importlib.util.module_from_spec(spec)
sys.modules["analyze_serial_log"] = analyze_serial_log
spec.loader.exec_module(analyze_serial_log)


def write_log(path: Path, lines):
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_analyze_gateway_rf_mqtt_and_no_buffer_burst(tmp_path):
    log = tmp_path / "gateway.txt"
    lines = [
        "I (100) iot-gw: IoT-Home Gateway v3.0 (Zigbee)",
        "I (200) iot-gw: Gateway ready!",
        "I (300) iot-gw: WiFi OK, IP: 192.168.1.10",
        "I (400) iot-gw: MQTT starting: mqtt://user:pass@mqtt.example:1883",
        "I (500) iot-gw: MQTT connected",
        "I (600) iot-gw: Zigbee: cached report node=0x82cb cluster=0x0402 value=2500",
        "I (700) iot-gw: MQTT -> nodes/zb-82cb/telemetry: "
            "{\"data\":{\"temperature\":25.0}}",
        "I (1000) iot-gw: MQTT rx: iot-home/gw-001/nodes/zb-82cb/cmd = "
            "{\"command\": \"rf_query_power\", \"payload\": {}}",
        "I (1050) iot-gw: MQTT rx: iot-home/gw-001/nodes/zb-82cb/cmd = "
            "{\"command\": \"diagnostic\", \"note\": \"No buffer\"}",
        "I (1100) iot-gw: RF power -> node=0x82cb cmd=1 power=0 mode=auto "
            "seq=1: ESP_OK",
        "I (1200) iot-gw: RF power: report accepted from unknown to auto/-10 dBm "
            "seq=1",
    ]
    lines.extend(
        f"\x1b[0;31mE ({1300 + index}) "
        "ZB_ESP_MAC: No buffer, handle it\x1b[0m"
        for index in range(25)
    )
    write_log(log, lines)

    result = analyze_serial_log.analyze(log, "auto", max_examples=3)

    assert result["role"] == "gateway"
    assert result["lines"] == len(lines)
    assert result["counts"]["telemetry_published"] == 1
    assert result["counts"]["zigbee_cached_reports"] == 1
    assert result["counts"]["mac_no_buffer"] == 25
    assert result["mqtt_commands"] == {
        "rf_query_power": 1,
        "diagnostic": 1,
    }
    assert result["zigbee_clusters"] == {"0x0402": 1}
    assert result["mac_no_buffer"]["total"] == 25
    assert result["mac_no_buffer"]["max_events_in_60s"] == 25
    assert len(result["mac_no_buffer"]["significant_bursts"]) == 1
    assert result["mac_no_buffer"]["significant_bursts"][0]["count"] == 25

    rf_sources = [event["source"] for event in result["rf_sequence_tail"]]
    assert "gateway" in rf_sources
    assert "gateway_report" in rf_sources

    starting_example = result["examples"]["mqtt_starting"][0]["text"]
    assert "pass" not in starting_example
    assert "mqtt://***@mqtt.example:1883" in starting_example


def test_analyze_node_report_stats_and_rf_sequence(tmp_path):
    log = tmp_path / "node.txt"
    lines = [
        "I (100) zb-sensor: Zigbee startup TX power target=-10 dBm before stack start",
        "I (200) zb-sensor: RF power cmd=3 power=-10 mode=0 seq=1: ESP_OK",
        "I (300) zb-sensor: REPORT_STATS: rounds=1 attempts=3 queued=3 "
            "queue_fail=0 aps_ok=3 aps_fail=0",
    ]
    write_log(log, lines)

    result = analyze_serial_log.analyze(log, "auto", max_examples=2)

    assert result["role"] == "node"
    assert result["counts"]["node_report_stats"] == 1
    assert result["node_last_report_stats"]["rounds"] == "1"
    assert result["node_last_report_stats"]["aps_fail"] == "0"
    assert result["rf_sequence_tail"][0] == {
        "cmd": "3",
        "power": "-10",
        "mode": "0",
        "seq": "1",
        "result": "ESP_OK",
        "line": 2,
        "uptime_ms": 200,
        "source": "node",
    }


def test_detect_uptime_reset_from_two_boot_segments(tmp_path):
    log = tmp_path / "reset.txt"
    lines = [
        f"E ({80000 + index}) ZB_ESP_MAC: No buffer, handle it"
        for index in range(15)
    ]
    lines.extend([
        "I (90000) iot-gw: Gateway ready!",
        "ESP-ROM:esp32c6-20220919",
        "rst:0x3 (SW_RESET)",
        "I (100) iot-gw: IoT-Home Gateway v3.0 (Zigbee)",
    ])
    lines.extend(
        f"E ({200 + index}) ZB_ESP_MAC: No buffer, handle it"
        for index in range(5)
    )
    lines.append("I (300) iot-gw: assert failed")
    write_log(log, lines)

    result = analyze_serial_log.analyze(log, "gateway", max_examples=1)

    assert result["uptime"]["detected_uptime_resets"] == 1
    assert result["uptime"]["span_s"] == 10.2
    assert result["counts"]["mac_no_buffer"] == 20
    assert result["counts"]["assert_failed"] == 1
    assert len(result["mac_no_buffer"]["significant_bursts"]) == 1
    assert result["mac_no_buffer"]["significant_bursts"][0]["count"] == 15
    text = analyze_serial_log.format_text(result)
    assert "启动时间回跳" in text
    assert "断言" in text


def test_invalid_mqtt_payload_shape(tmp_path):
    log = tmp_path / "invalid.txt"
    long_command = "x" * 65
    lines = [
        "I (100) iot-gw: MQTT rx: topic = [1, 2, 3]",
        "I (200) iot-gw: MQTT rx: topic = {\"command\": 123}",
        "I (300) iot-gw: MQTT rx: topic = {\"command\": null}",
        "I (400) iot-gw: MQTT rx: topic = {\"command\": \"\"}",
        f"I (500) iot-gw: MQTT rx: topic = "
            f"{{\"command\": \"{long_command}\"}}",
    ]
    write_log(log, lines)

    result = analyze_serial_log.analyze(log, "gateway", max_examples=1)

    assert result["mqtt_commands"] == {"invalid": 5}
