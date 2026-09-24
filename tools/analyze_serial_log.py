#!/usr/bin/env python3
"""流式分析 ESP32-C6 网关/节点串口日志。

设计目标：
- 不一次性载入大文件，适合数小时甚至数百 MB 的串口日志；
- 只使用 Python 标准库；
- 输出重启、断言、Zigbee、MQTT/Wi‑Fi、RF 指令、MAC buffer 风暴等摘要；
- 不改变设备、网络或服务器状态。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ANSI_CSI_RE = re.compile(r"\x1B(?:\[[0-?]*[ -/]*[@-~]|[@-Z\\-_])")
LOG_LINE_RE = re.compile(r"^([IWE]) \((\d+)\)\s+(?:[^:]+:\s*)?(.*)$")
RF_GATEWAY_RESULT_RE = re.compile(
    r"RF power -> node=(?P<node>0x[0-9a-fA-F]+)\s+"
    r"cmd=(?P<cmd>\d+)\s+power=(?P<power>-?\d+)\s+"
    r"mode=(?P<mode>\w+)\s+seq=(?P<seq>\d+):\s+(?P<result>\S+)"
)
RF_GATEWAY_REPORT_RE = re.compile(
    r"RF power: report accepted from \S+ to "
    r"(?P<mode>\w+)/(?P<power>-?\d+) dBm seq=(?P<seq>\d+)"
)
RF_NODE_RESULT_RE = re.compile(
    r"RF power cmd=(?P<cmd>\d+)\s+power=(?P<power>-?\d+)\s+"
    r"mode=(?P<mode>\d+)\s+seq=(?P<seq>\d+):\s+(?P<result>\S+)"
)
MAC_NO_BUFFER_RE = re.compile(r"^[IWE] \(\d+\) ZB_ESP_MAC: No buffer\b")
MAX_COMMAND_LEN = 64
CACHED_REPORT_RE = re.compile(
    r"cached report node=(?P<node>0x[0-9a-fA-F]+)\s+"
    r"cluster=(?P<cluster>0x[0-9a-fA-F]+)"
)
REPORT_STATS_RE = re.compile(r"REPORT_STATS:\s*(?P<body>.*)$")
URI_USERINFO_RE = re.compile(r"(\b[a-zA-Z][a-zA-Z0-9+.-]*://)[^/\s@]+@")


@dataclass
class EventExamples:
    limit: int = 5
    examples: list[dict[str, Any]] = field(default_factory=list)

    def add(self, line_no: int, uptime_ms: int | None, text: str) -> None:
        if len(self.examples) < self.limit:
            self.examples.append(
                {
                    "line": line_no,
                    "uptime_ms": uptime_ms,
                    "text": redact(text.strip())[:500],
                }
            )


@dataclass
class Burst:
    start_line: int
    start_ms: int
    end_line: int = 0
    end_ms: int = 0
    count: int = 1

    def as_dict(self) -> dict[str, Any]:
        duration_s = None
        if self.start_ms is not None and self.end_ms is not None:
            duration_s = round((self.end_ms - self.start_ms) / 1000.0, 3)
        return {
            "count": self.count,
            "start_line": self.start_line,
            "end_line": self.end_line,
            "start_uptime_ms": self.start_ms,
            "end_uptime_ms": self.end_ms,
            "duration_s": duration_s,
        }


def redact(text: str) -> str:
    return URI_USERINFO_RE.sub(r"\1***@", text)


def parse_kv_stats(body: str) -> dict[str, str]:
    stats: dict[str, str] = {}
    for item in body.split():
        if "=" in item:
            key, value = item.split("=", 1)
            stats[key] = value
    return stats


def detect_role(line: str) -> str | None:
    if "iot-gw:" in line or "IoT-Home Gateway" in line:
        return "gateway"
    if "zb-sensor:" in line:
        return "node"
    return None


def analyze(path: Path, role_hint: str, max_examples: int) -> dict[str, Any]:
    line_count = 0
    role_votes: Counter[str] = Counter()
    first_uptime: int | None = None
    last_uptime: int | None = None
    segment_start_uptime: int | None = None
    total_uptime_ms = 0
    uptime_resets = 0
    previous_uptime: int | None = None

    level_counts: Counter[str] = Counter()
    counters: Counter[str] = Counter()
    cluster_counts: Counter[str] = Counter()
    mqtt_commands: Counter[str] = Counter()
    command_sequence: deque[dict[str, Any]] = deque(maxlen=50)
    rf_sequence: deque[dict[str, Any]] = deque(maxlen=100)

    examples: dict[str, EventExamples] = {}

    def example(group: str) -> EventExamples:
        if group not in examples:
            examples[group] = EventExamples(limit=max_examples)
        return examples[group]

    def count(group: str, line_no: int, uptime_ms: int | None, text: str) -> None:
        counters[group] += 1
        example(group).add(line_no, uptime_ms, text)

    current_buffer_burst: Burst | None = None
    significant_buffer_bursts: list[dict[str, Any]] = []
    max_buffer_per_60s = 0
    sliding_window: deque[tuple[int, int]] = deque()

    first_stats: dict[str, str] | None = None
    last_stats: dict[str, str] | None = None

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line_count += 1
            line = ANSI_CSI_RE.sub("", raw_line).rstrip("\r\n")
            detected = detect_role(line)
            if detected:
                role_votes[detected] += 1

            uptime_ms: int | None = None
            match = LOG_LINE_RE.match(line)
            if match:
                level, uptime_text, message = match.groups()
                uptime_ms = int(uptime_text)
                level_counts[level] += 1
                if first_uptime is None:
                    first_uptime = uptime_ms
                if segment_start_uptime is None:
                    segment_start_uptime = uptime_ms
                if previous_uptime is not None and uptime_ms + 1000 < previous_uptime:
                    uptime_resets += 1
                    sliding_window.clear()
                    if (current_buffer_burst is not None and
                            current_buffer_burst.count >= 10):
                        significant_buffer_bursts.append(
                            current_buffer_burst.as_dict()
                        )
                    current_buffer_burst = None
                    total_uptime_ms += (
                        previous_uptime - segment_start_uptime
                    )
                    segment_start_uptime = uptime_ms
                previous_uptime = uptime_ms
                last_uptime = uptime_ms
                haystack = message
            else:
                haystack = line

            lower = haystack.lower()

            is_mac_no_buffer = MAC_NO_BUFFER_RE.match(line) is not None
            if is_mac_no_buffer:
                counters["mac_no_buffer"] += 1
                if uptime_ms is not None:
                    sliding_window.append((line_count, uptime_ms))
                    while sliding_window and uptime_ms - sliding_window[0][1] > 60_000:
                        sliding_window.popleft()
                    max_buffer_per_60s = max(max_buffer_per_60s, len(sliding_window))

                    if current_buffer_burst is None:
                        current_buffer_burst = Burst(line_count, uptime_ms)
                    elif uptime_ms - current_buffer_burst.end_ms <= 60_000:
                        current_buffer_burst.count += 1
                    else:
                        ended = current_buffer_burst
                        if ended.count >= 10:
                            significant_buffer_bursts.append(ended.as_dict())
                        current_buffer_burst = Burst(line_count, uptime_ms)
                    current_buffer_burst.end_line = line_count
                    current_buffer_burst.end_ms = uptime_ms

            if "assert failed" in lower:
                count("assert_failed", line_count, uptime_ms, haystack)
            if "guru meditation" in lower:
                count("panic_guru", line_count, uptime_ms, haystack)
            if "abort()" in lower:
                count("abort", line_count, uptime_ms, haystack)
            if "backtrace:" in lower:
                count("backtrace", line_count, uptime_ms, haystack)
            if "rebooting" in lower or "restarting" in lower:
                count("reboot_log", line_count, uptime_ms, haystack)
            if "panic" in lower:
                count("panic", line_count, uptime_ms, haystack)

            if "rst:0x" in lower or "reset reason" in lower:
                count("reset_reason", line_count, uptime_ms, haystack)
            if "boot: loaded app" in lower:
                count("boot_loaded_app", line_count, uptime_ms, haystack)
            if "iot-home gateway v" in lower:
                count("gateway_banner", line_count, uptime_ms, haystack)
            if "gateway ready" in lower:
                count("gateway_ready", line_count, uptime_ms, haystack)

            if "device joined" in lower:
                count("zigbee_device_joined", line_count, uptime_ms, haystack)
            if "device left" in lower or "device leave" in lower:
                count("zigbee_device_left", line_count, uptime_ms, haystack)
            if "zigbee: connected" in lower:
                count("zigbee_connected", line_count, uptime_ms, haystack)
            if "startup/rejoin failed" in lower:
                count("zigbee_rejoin_failed", line_count, uptime_ms, haystack)
            if "nlme status indication" in lower:
                count("zigbee_nlme_status", line_count, uptime_ms, haystack)
            if "zigbee signal:" in lower:
                counters["zigbee_signal"] += 1

            if "wifi ok, ip:" in lower:
                count("wifi_ok", line_count, uptime_ms, haystack)
            if "wifi" in lower and "disconnect" in lower:
                count("wifi_disconnect", line_count, uptime_ms, haystack)
            if "mqtt starting:" in lower:
                count("mqtt_starting", line_count, uptime_ms, haystack)
            if "mqtt connected" in lower:
                count("mqtt_connected", line_count, uptime_ms, haystack)
            if "mqtt" in lower and "disconnect" in lower:
                count("mqtt_disconnect", line_count, uptime_ms, haystack)
            if "mqtt ->" in lower and "telemetry" in lower:
                counters["telemetry_published"] += 1
            if "mqtt duplicate suppressed" in lower:
                counters["telemetry_duplicate_suppressed"] += 1

            cached_match = CACHED_REPORT_RE.search(haystack)
            if cached_match:
                counters["zigbee_cached_reports"] += 1
                cluster_counts[cached_match.group("cluster").lower()] += 1

            if haystack.startswith("MQTT rx:"):
                _, _, payload_text = haystack.partition(" = ")
                command = "unparseable"
                payload_text = payload_text.strip()
                if payload_text:
                    try:
                        payload = json.loads(payload_text)
                    except (json.JSONDecodeError, RecursionError):
                        command = "unparseable"
                    else:
                        if not isinstance(payload, dict):
                            command = "invalid"
                        else:
                            raw_command = payload.get("command")
                            if not isinstance(raw_command, str):
                                command = "invalid"
                            elif (not raw_command or
                                    len(raw_command) > MAX_COMMAND_LEN):
                                command = "invalid"
                            else:
                                command = raw_command
                mqtt_commands[command] += 1
                if len(command_sequence) == command_sequence.maxlen:
                    command_sequence.popleft()
                command_sequence.append(
                    {"line": line_count, "uptime_ms": uptime_ms, "command": command}
                )

            gateway_rf = RF_GATEWAY_RESULT_RE.search(haystack)
            if gateway_rf:
                item = gateway_rf.groupdict()
                item["line"] = line_count
                item["uptime_ms"] = uptime_ms
                item["source"] = "gateway"
                if len(rf_sequence) == rf_sequence.maxlen:
                    rf_sequence.popleft()
                rf_sequence.append(item)

            gateway_report = RF_GATEWAY_REPORT_RE.search(haystack)
            if gateway_report:
                item = gateway_report.groupdict()
                item["line"] = line_count
                item["uptime_ms"] = uptime_ms
                item["source"] = "gateway_report"
                if len(rf_sequence) == rf_sequence.maxlen:
                    rf_sequence.popleft()
                rf_sequence.append(item)

            node_rf = RF_NODE_RESULT_RE.search(haystack)
            if node_rf:
                item = node_rf.groupdict()
                item["line"] = line_count
                item["uptime_ms"] = uptime_ms
                item["source"] = "node"
                if len(rf_sequence) == rf_sequence.maxlen:
                    rf_sequence.popleft()
                rf_sequence.append(item)

            stats_match = REPORT_STATS_RE.search(haystack)
            if stats_match:
                stats = parse_kv_stats(stats_match.group("body"))
                if first_stats is None:
                    first_stats = stats
                last_stats = stats
                counters["node_report_stats"] += 1

    if role_hint != "auto":
        role = role_hint
    elif role_votes:
        role = role_votes.most_common(1)[0][0]
    else:
        role = "unknown"

    uptime_span_s = None
    if (first_uptime is not None and last_uptime is not None and
            segment_start_uptime is not None):
        total_uptime_ms += last_uptime - segment_start_uptime
        uptime_span_s = round(total_uptime_ms / 1000.0, 3)

    if (current_buffer_burst is not None and
            current_buffer_burst.count >= 10):
        significant_buffer_bursts.append(current_buffer_burst.as_dict())

    return {
        "file": str(path.resolve()),
        "file_size_bytes": path.stat().st_size,
        "role": role,
        "role_votes": dict(role_votes),
        "lines": line_count,
        "log_levels": dict(level_counts),
        "uptime": {
            "first_ms": first_uptime,
            "last_ms": last_uptime,
            "span_s": uptime_span_s,
            "detected_uptime_resets": uptime_resets,
        },
        "counts": dict(counters),
        "zigbee_clusters": dict(cluster_counts),
        "mqtt_commands": dict(mqtt_commands),
        "command_sequence_tail": list(command_sequence),
        "rf_sequence_tail": list(rf_sequence),
        "mac_no_buffer": {
            "total": counters.get("mac_no_buffer", 0),
            "max_events_in_60s": max_buffer_per_60s,
            "significant_bursts": significant_buffer_bursts,
        },
        "node_first_report_stats": first_stats,
        "node_last_report_stats": last_stats,
        "examples": {name: value.examples for name, value in examples.items()},
    }


def format_text(result: dict[str, Any]) -> str:
    lines: list[str] = []
    lines.append(f"文件：{result['file']}")
    lines.append(
        f"角色：{result['role']}；行数：{result['lines']}；"
        f"大小：{result['file_size_bytes']} 字节"
    )
    uptime = result["uptime"]
    lines.append(
        f"日志覆盖：{uptime['span_s']} 秒；检测到 uptime 回跳："
        f"{uptime['detected_uptime_resets']} 次"
    )
    lines.append(f"日志级别：{result['log_levels']}")
    lines.append("")
    lines.append("关键计数：")
    for key, value in sorted(result["counts"].items()):
        lines.append(f"  - {key}: {value}")

    buffer_info = result["mac_no_buffer"]
    lines.append("")
    lines.append(
        "MAC no-buffer："
        f"总计 {buffer_info['total']}，60 秒窗口峰值 "
        f"{buffer_info['max_events_in_60s']}，显著突发 "
        f"{len(buffer_info['significant_bursts'])} 个"
    )
    for burst in buffer_info["significant_bursts"][:5]:
        lines.append(f"  - {burst}")

    if result["mqtt_commands"]:
        lines.append("")
        lines.append(f"MQTT 命令计数：{result['mqtt_commands']}")
    if result["zigbee_clusters"]:
        lines.append(f"Zigbee cluster 计数：{result['zigbee_clusters']}")
    if result["node_last_report_stats"]:
        lines.append("")
        lines.append(f"节点最新 REPORT_STATS：{result['node_last_report_stats']}")

    lines.append("")
    lines.append("结论提示：")
    warnings = []
    counts = result["counts"]
    if counts.get("assert_failed") or counts.get("panic_guru") or counts.get("abort"):
        warnings.append("发现断言/panic/abort，需要优先定位。")
    if buffer_info["max_events_in_60s"] >= 20:
        warnings.append("出现较密集 MAC no-buffer 窗口，需要结合 Zigbee 帧与健康重启日志判断。")
    if uptime["detected_uptime_resets"] > 0:
        warnings.append("日志中出现启动时间回跳，可能存在重启或多段捕获。")
    if not warnings:
        warnings.append("未从上述规则中发现明确危险信号；仍需结合业务命令和遥测完整性验收。")
    lines.extend(f"  - {item}" for item in warnings)
    return "\n".join(lines)


def main() -> int:
    # Keep Chinese output stable on Windows consoles and CI capture without
    # requiring callers to set PYTHONIOENCODING.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(
        description="流式分析 ESP32-C6 网关/节点串口长测日志"
    )
    parser.add_argument("logfile", type=Path, help="串口日志文件路径")
    parser.add_argument(
        "--role",
        choices=("auto", "gateway", "node"),
        default="auto",
        help="日志角色，默认自动识别",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="输出格式",
    )
    def bounded_example_count(value: str) -> int:
        try:
            count = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                "示例数量必须是 0 到 20 之间的整数"
            ) from exc
        if not 0 <= count <= 20:
            raise argparse.ArgumentTypeError(
                "示例数量必须是 0 到 20 之间的整数"
            )
        return count

    parser.add_argument(
        "--max-examples",
        type=bounded_example_count,
        default=5,
        help="每类事件最多保留的示例数（0-20）",
    )
    args = parser.parse_args()

    try:
        result = analyze(args.logfile, args.role, args.max_examples)
    except OSError as exc:
        print(f"无法读取日志文件：{exc}", file=sys.stderr)
        return 2
    if args.format == "json":
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print(format_text(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
