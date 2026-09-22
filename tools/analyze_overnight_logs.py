#!/usr/bin/env python3
"""Analyze paired ESP32-C6 node/gateway overnight serial captures.

Usage:
  python tools/analyze_overnight_logs.py node.log gateway.log \
      --json overnight.json --markdown overnight.md

The parser tolerates binary/NUL bytes and partial captures. It works with the
current firmware logs, especially node ``REPORT_STATS`` lines and gateway
``MQTT -> ...`` lines.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Optional

ESP_LINE_RE = re.compile(r"^[IWE] \((\d+)\) ([^:]+): (.*)$")
RESET_RE = re.compile(r"^rst:0x[0-9a-fx]+ \(([^)]+)\)")
KV_RE = re.compile(r"(\w+)=([^\s]+)")
RF_CHANGE_RE = re.compile(r"^RF power: .+ -> -?\d+ dBm")
RF_CMD_RE = re.compile(r"^RF power cmd=(\d+)")

ERROR_TERMS = (
    "assert failed",
    "backtrace:",
    "guru meditation",
    "stack canary",
    "abort()",
    "fatal exception",
)

NODE_STAT_KEYS = (
    "rounds",
    "repairs",
    "i2c_th_fail",
    "i2c_lux_fail",
    "i2c_resets",
    "i2c_reinits",
    "attempts",
    "queued",
    "queue_fail",
    "items_failed",
    "aps_ok",
    "aps_fail",
    "unavail",
    "recovered",
    "candidate",
    "probe_resteer",
    "probes",
    "probe_aps_fail",
    "th_invalid",
    "lux_invalid",
    "th_streak",
    "lux_streak",
)


@dataclass
class BasicCounters:
    total_lines: int = 0
    esp_lines: int = 0
    resets: int = 0
    reset_reasons: dict[str, int] = field(default_factory=dict)
    warnings: int = 0
    errors: int = 0
    panics: int = 0
    panic_samples: list[str] = field(default_factory=list)
    first_uptime_ms: Optional[int] = None
    last_uptime_ms: Optional[int] = None
    # Compatibility field: duration of the latest boot segment.
    observed_ms: int = 0
    last_boot_observed_ms: int = 0
    total_observed_ms: int = 0
    boot_segments: list[dict[str, Any]] = field(default_factory=list)
    gaps_over_30s: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class NodeReport:
    counters: BasicCounters = field(default_factory=BasicCounters)
    data_samples: int = 0
    report_sends: int = 0
    report_send_skips: int = 0
    last_stats: dict[str, Any] = field(default_factory=dict)
    max_stats: dict[str, float] = field(default_factory=dict)
    rejoin_failures: int = 0
    steering_successes: int = 0
    link_lost: int = 0
    device_started: int = 0
    rf_power_changes: int = 0
    rf_commands: dict[str, int] = field(default_factory=dict)


@dataclass
class GatewayReport:
    counters: BasicCounters = field(default_factory=BasicCounters)
    mqtt_forwarded: int = 0
    duplicate_suppressed: int = 0
    rf_samples: int = 0
    rf_clusters: dict[str, int] = field(default_factory=dict)
    rf_policy_sent: int = 0
    rf_reports_accepted: int = 0
    rf_power_keeps: int = 0
    mqtt_connections: int = 0
    mqtt_disconnections: int = 0
    wifi_connections: int = 0
    wifi_disconnections: int = 0
    device_joined: int = 0
    device_announced: int = 0
    device_left: int = 0
    unavailable_events: int = 0


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8", errors="ignore")


def parse_int(value: str) -> Optional[float]:
    # Values such as candidate=0/0 keep their numerator.
    value = value.split("/", 1)[0]
    try:
        number: float = int(value)
    except ValueError:
        try:
            number = float(value)
        except ValueError:
            return None
    return number


def recalculate_boot_totals(counters: BasicCounters) -> None:
    counters.total_observed_ms = sum(
        int(segment["observed_ms"]) for segment in counters.boot_segments
    )
    if counters.boot_segments:
        counters.last_boot_observed_ms = int(
            counters.boot_segments[-1]["observed_ms"]
        )
    else:
        counters.last_boot_observed_ms = 0
    counters.observed_ms = counters.last_boot_observed_ms


def update_uptime(counters: BasicCounters, uptime: int) -> None:
    previous = counters.last_uptime_ms
    # A large downward move means a new boot segment even if the ROM reset
    # banner was interleaved/missing from the captured stream.
    starts_new_segment = (
        previous is None
        or uptime + 1000 < previous
    )

    if starts_new_segment:
        counters.boot_segments.append(
            {
                "index": len(counters.boot_segments) + 1,
                "first_uptime_ms": uptime,
                "last_uptime_ms": uptime,
                "observed_ms": uptime,
            }
        )
    else:
        segment = counters.boot_segments[-1]
        last_in_segment = int(segment["last_uptime_ms"])
        if uptime > last_in_segment:
            gap = uptime - last_in_segment
            if gap > 30_000:
                counters.gaps_over_30s.append(
                    {
                        "boot_segment": segment["index"],
                        "from_ms": last_in_segment,
                        "to_ms": uptime,
                        "gap_ms": gap,
                    }
                )
        segment["last_uptime_ms"] = uptime
        segment["observed_ms"] = max(int(segment["observed_ms"]), uptime)

    counters.last_uptime_ms = uptime
    if counters.first_uptime_ms is None:
        counters.first_uptime_ms = uptime
    recalculate_boot_totals(counters)


def update_basic(counters: BasicCounters, raw_line: str, uptime: Optional[int]) -> None:
    counters.total_lines += 1
    if uptime is not None:
        counters.esp_lines += 1
        update_uptime(counters, uptime)

    reset = RESET_RE.match(raw_line)
    if reset:
        counters.resets += 1
        reason = reset.group(1)
        counters.reset_reasons[reason] = counters.reset_reasons.get(reason, 0) + 1

    if raw_line.startswith("W "):
        counters.warnings += 1
    if raw_line.startswith("E ") or raw_line.lower().startswith("error"):
        counters.errors += 1

    lowered = raw_line.lower()
    if any(term in lowered for term in ERROR_TERMS):
        counters.panics += 1
        if len(counters.panic_samples) < 10:
            counters.panic_samples.append(raw_line[:300])


def parse_node(text: str) -> NodeReport:
    report = NodeReport()
    for raw in text.splitlines():
        line = raw.strip("\x00\r\n ")
        match = ESP_LINE_RE.match(line)
        uptime = int(match.group(1)) if match else None
        message = match.group(3) if match else line
        update_basic(report.counters, line, uptime)

        if message.startswith('DATA: '):
            report.data_samples += 1
        if 'REPORT_SEND:' in message:
            if ' skipped ' in message:
                report.report_send_skips += 1
            else:
                report.report_sends += 1
        if 'Rejoin failure' in line:
            report.rejoin_failures += 1
        if 'Connected to network!' in message:
            report.steering_successes += 1
        if 'Zigbee: Device started' in message:
            report.device_started += 1
        if 'link lost signal=' in message:
            report.link_lost += 1
        if RF_CHANGE_RE.match(message):
            report.rf_power_changes += 1
        cmd_match = RF_CMD_RE.match(message)
        if cmd_match:
            command = cmd_match.group(1)
            report.rf_commands[command] = report.rf_commands.get(command, 0) + 1

        if 'REPORT_STATS:' in message:
            parsed: dict[str, Any] = {}
            for key, value in KV_RE.findall(message):
                if key in NODE_STAT_KEYS:
                    number = parse_int(value)
                    if number is not None:
                        parsed[key] = int(number)
            # The latest stats represent cumulative counters in the current boot.
            report.last_stats.update(parsed)
            for key, value in parsed.items():
                if isinstance(value, (int, float)):
                    report.max_stats[key] = max(
                        report.max_stats.get(key, float(value)), float(value)
                    )
    return report


def parse_gateway(text: str) -> GatewayReport:
    report = GatewayReport()
    for raw in text.splitlines():
        line = raw.strip("\x00\r\n ")
        match = ESP_LINE_RE.match(line)
        uptime = int(match.group(1)) if match else None
        message = match.group(3) if match else line
        update_basic(report.counters, line, uptime)

        if message.startswith('MQTT -> '):
            report.mqtt_forwarded += 1
        if 'MQTT duplicate suppressed' in message:
            report.duplicate_suppressed += 1
        if message.startswith('RF sample '):
            report.rf_samples += 1
            cluster = re.search(r'cluster=(0x[0-9a-f]+)', message)
            if cluster:
                key = cluster.group(1)
                report.rf_clusters[key] = report.rf_clusters.get(key, 0) + 1
        if 'RF power ->' in message:
            report.rf_policy_sent += 1
        if 'RF power: report accepted' in message:
            report.rf_reports_accepted += 1
        if 'RF power: keep ' in message:
            report.rf_power_keeps += 1
        if message == 'MQTT connected':
            report.mqtt_connections += 1
        if message == 'MQTT disconnected':
            report.mqtt_disconnections += 1
        if message.startswith('WiFi OK'):
            report.wifi_connections += 1
        if 'WiFi disconnected' in message or 'WiFi lost' in message:
            report.wifi_disconnections += 1
        if 'Device joined!' in message:
            report.device_joined += 1
        if 'Device announced again!' in message:
            report.device_announced += 1
        if 'Device left!' in message:
            report.device_left += 1
        if 'link/service unavailable' in message:
            report.unavailable_events += 1
    return report


def percentage(part: float, total: float) -> Optional[float]:
    if total <= 0:
        return None
    return round(100.0 * part / total, 3)


def build_summary(node: NodeReport, gateway: GatewayReport) -> dict[str, Any]:
    aps_total = node.last_stats.get('aps_ok', 0) + node.last_stats.get('aps_fail', 0)
    return {
        "node": asdict(node),
        "gateway": asdict(gateway),
        "derived": {
            "node_aps_success_pct": percentage(
                node.last_stats.get('aps_ok', 0), aps_total
            ),
            "node_queue_fail_pct": percentage(
                node.last_stats.get('queue_fail', 0),
                node.last_stats.get('attempts', 0),
            ),
            "node_i2c_fail_total": (
                node.last_stats.get('i2c_th_fail', 0)
                + node.last_stats.get('i2c_lux_fail', 0)
            ),
        },
    }


def fmt_ms(ms: int) -> str:
    seconds = ms // 1000
    hours, rem = divmod(seconds, 3600)
    minutes, sec = divmod(rem, 60)
    return f"{hours}h {minutes}m {sec}s"


def render_markdown(summary: dict[str, Any]) -> str:
    node = summary["node"]
    gw = summary["gateway"]
    derived = summary["derived"]
    node_c = node["counters"]
    gw_c = gw["counters"]
    lines = [
        "# 整夜链路日志分析",
        "",
        "## 总览",
        "",
        "| 项目 | 节点 | 网关 |",
        "| --- | ---: | ---: |",
        f"| 有效 ESP 日志行 | {node_c['esp_lines']} | {gw_c['esp_lines']} |",
        f"| 启动段数 | {len(node_c['boot_segments'])} | {len(gw_c['boot_segments'])} |",
        f"| 最后一次启动时长 | {fmt_ms(node_c['last_boot_observed_ms'])} | {fmt_ms(gw_c['last_boot_observed_ms'])} |",
        f"| 所有启动段累计时长 | {fmt_ms(node_c['total_observed_ms'])} | {fmt_ms(gw_c['total_observed_ms'])} |",
        f"| 重启 | {node_c['resets']} | {gw_c['resets']} |",
        f"| panic/assert 线索 | {node_c['panics']} | {gw_c['panics']} |",
        f"| warning | {node_c['warnings']} | {gw_c['warnings']} |",
        "",
        "## 节点",
        "",
        f"- 数据采样：{node['data_samples']} 次；REPORT_SEND：{node['report_sends']} 次，跳过：{node['report_send_skips']} 次。",
        f"- 最近一段启动的 APS：成功 {node['last_stats'].get('aps_ok', 0)}，失败 {node['last_stats'].get('aps_fail', 0)}，成功率 {derived['node_aps_success_pct']}%。",
        f"- 队列失败：{node['last_stats'].get('queue_fail', 0)}；I2C 失败合计：{derived['node_i2c_fail_total']}。",
        f"- Rejoin failure 日志：{node['rejoin_failures']}；link lost：{node['link_lost']}；入网成功提示：{node['steering_successes']}。",
        f"- 实际 RF 功率变化：{node['rf_power_changes']}；RF cmd 计数：`{node['rf_commands']}`。",
        "",
        "## 网关",
        "",
        f"- MQTT 转发遥测：{gw['mqtt_forwarded']} 条；重复抑制：{gw['duplicate_suppressed']} 条。",
        f"- RF samples：{gw['rf_samples']}，其中 cluster 计数：`{gw['rf_clusters']}`。",
        f"- RF policy 下发：{gw['rf_policy_sent']}；report accepted：{gw['rf_reports_accepted']}；keep：{gw['rf_power_keeps']}。",
        f"- MQTT connected/disconnected：{gw['mqtt_connections']}/{gw['mqtt_disconnections']}；Wi-Fi connected/disconnected：{gw['wifi_connections']}/{gw['wifi_disconnections']}。",
        f"- joined/announced/left/unavailable：{gw['device_joined']}/{gw['device_announced']}/{gw['device_left']}/{gw['unavailable_events']}。",
        "",
        "## 启动段",
        "",
    ]
    for label, counters in (("节点", node_c), ("网关", gw_c)):
        segments = ", ".join(
            f"#{seg['index']}={fmt_ms(int(seg['observed_ms']))}"
            for seg in counters["boot_segments"]
        ) or "无"
        lines.append(f"- {label}：{segments}")
    lines.extend(["", "## 需要关注", ""])

    concerns: list[str] = []
    if node_c["panics"]:
        concerns.append(f"节点出现 {node_c['panics']} 条 panic/assert 线索。")
    if node_c["resets"] > 1:
        concerns.append(f"节点记录 {node_c['resets']} 次复位（含采集开始时复位）。")
    if node["last_stats"].get("queue_fail", 0):
        concerns.append("节点存在 queue_fail。")
    if derived["node_i2c_fail_total"]:
        concerns.append("节点存在 I2C 失败。")
    if gw_c["panics"]:
        concerns.append("网关出现 panic/assert 线索。")
    if not concerns:
        concerns.append("未发现自动规则可识别的问题。")
    lines.extend(f"- {item}" for item in concerns)
    lines.append("")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("node_log", type=Path)
    parser.add_argument("gateway_log", type=Path)
    parser.add_argument("--json", type=Path, help="write machine-readable JSON")
    parser.add_argument("--markdown", type=Path, help="write Markdown report")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    node = parse_node(read_text(args.node_log))
    gateway = parse_gateway(read_text(args.gateway_log))
    summary = build_summary(node, gateway)

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    markdown = render_markdown(summary)
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(markdown, encoding="utf-8")
    print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
