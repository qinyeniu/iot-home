#!/usr/bin/env python3
"""分析节点/网关双串口长稳定性测试日志。

只读取日志文件，不访问设备、不烧录、不改变网络。默认按 8 小时验收：

- PASS：目标时长满足，且未识别到严重问题或降级线索；
- DEGRADED：时长可能未满足，但已出现 APS/I2C/队列失败、观察缺口等；
- FAIL：出现断言、panic、重启、uptime 回跳、MAC no-buffer 风暴，或错误风暴叠加业务黑障；
- INCOMPLETE：未达到目标时长，且尚未出现明确降级或失败线索。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, deque
from pathlib import Path
from typing import Any

ANSI_RE = re.compile(r"\x1B(?:\[[0-?]*[ -/]*[@-~]|[@-Z\\-_])")
ESP_RE = re.compile(
    r"^(?P<level>[IWE]) \((?P<uptime>\d+)\)\s+"
    r"(?P<tag>[^:]+):\s*(?P<text>.*)$"
)
REPORT_STATS_RE = re.compile(r"REPORT_STATS:\s*(?P<body>.*)$")
MAC_NO_BUFFER_RE = re.compile(r"^No buffer\b")
TELEMETRY_RE = re.compile(r"MQTT -> \S*telemetry\b")
DUPLICATE_RE = re.compile(r"MQTT duplicate suppressed\b")
CACHED_REPORT_RE = re.compile(r"cached report node=")
RF_SAMPLE_RE = re.compile(r"\bRF sample\b")
RF_SEND_RE = re.compile(r"\bRF power -> node=")
RF_SEND_RESULT_RE = re.compile(
    r"\bRF power -> node=.*:\s*(?P<result>ESP_[A-Z0-9_]+)\s*$"
)
RF_ACCEPTED_RE = re.compile(r"\bRF power: report accepted\b")
RF_KEEP_RE = re.compile(r"\bRF power: keep\b")
NODE_REJOIN_FAILURE_RE = re.compile(r"\bRejoin failure:")
WIFI_MF_NULL_RE = re.compile(r"^m f null$")
URI_USERINFO_RE = re.compile(r"(\b[A-Za-z][A-Za-z0-9+.-]*://)[^\s/@]+@")
URL_QUERY_SECRET_RE = re.compile(
    r"(?i)([?&](?:access_token|refresh_token|token|api[_-]?key|apikey|"
    r"secret|password|passwd|pwd)=)[^&#\s]+"
)
SENSITIVE_KV_RE = re.compile(
    r"(?i)\b[\"']?(password|passwd|pwd|token|api[_-]?key|apikey|secret)"
    r"[\"']?\s*[:=]\s*[\"']?[^\"'\s,}]+"
)
EVENT_SAMPLE_LIMIT = 20

# 一行只记一次严重事件，避免同一 backtrace 被多个关键词重复计数。
FATAL_PATTERNS = [
    re.compile(pattern, re.IGNORECASE)
    for pattern in (
        r"assert failed",
        r"guru meditation",
        r"\bpanic\b",
        r"fatal exception",
        r"abort\(\)",
        r"stack canary",
        r"vPortExitCritical",
        r"reentered critical",
        r"^backtrace:",
    )
]

# 采集开始前已经存在的 boot banner 只作为起点信息；开始观察后再次出现则是重启。
RESTART_PATTERNS = [
    re.compile(pattern, re.IGNORECASE)
    for pattern in (
        r"^rst:0x",
        r"^ESP-ROM:",
        r"\bboot:0x",
        r"\besp_restart\b",
        r"health restart",
        r"rebooting",
    )
]

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

DEGRADED_STAT_KEYS = (
    "aps_fail",
    "repairs",
    "i2c_th_fail",
    "i2c_lux_fail",
    "i2c_resets",
    "i2c_reinits",
    "queue_fail",
    "items_failed",
    "probe_aps_fail",
    "th_invalid",
    "lux_invalid",
)


def redact(text: str) -> str:
    """移除日志样本中常见的凭据，避免进入 Markdown/JSON。"""
    text = URI_USERINFO_RE.sub(r"\1***@", text)
    text = URL_QUERY_SECRET_RE.sub(r"\1***", text)
    text = SENSITIVE_KV_RE.sub(r"\1=***", text)
    return text


def parse_stat_value(value: str) -> int | None:
    # candidate=0/0、probes=3/3 这类字段保留分子。
    value = value.split("/", 1)[0]
    try:
        return int(value)
    except ValueError:
        return None


def parse_report_stats(body: str) -> dict[str, int]:
    stats: dict[str, int] = {}
    for item in body.split():
        if "=" not in item:
            continue
        key, raw_value = item.split("=", 1)
        value = parse_stat_value(raw_value)
        if value is not None:
            stats[key] = value
    return stats


def stat_deltas(first: dict[str, int], last: dict[str, int]) -> dict[str, int]:
    deltas: dict[str, int] = {}
    for key in NODE_STAT_KEYS:
        if key in first and key in last:
            deltas[key] = last[key] - first[key]
    for key in first.keys() | last.keys():
        if key not in deltas and key in first and key in last:
            deltas[key] = last[key] - first[key]
    return deltas


def _event(line_no: int, uptime_ms: int | None, text: str) -> dict[str, Any]:
    return {
        "line": line_no,
        "uptime_ms": uptime_ms,
        "text": redact(text.strip())[:500],
    }


def add_sample(
    samples: list[dict[str, Any]],
    event: dict[str, Any],
    limit: int = EVENT_SAMPLE_LIMIT,
) -> None:
    """只保留固定数量样本；真实总数由独立计数字段记录。"""
    if len(samples) < limit:
        samples.append(event)


class TimedEventStorm:
    """按滑动窗口统计带 uptime 的重复事件。"""

    def __init__(
        self,
        event_type: str,
        window_s: float,
        threshold: int,
    ) -> None:
        self.event_type = event_type
        self.window_s = window_s
        self.threshold = threshold
        self.total = 0
        self.max_events_in_window = 0
        self.significant_burst_count = 0
        self.significant_bursts: list[dict[str, Any]] = []
        self._events: deque[tuple[int, int]] = deque()
        self._active = False

    def reset(self) -> None:
        """uptime 回跳后丢弃旧启动段事件。"""
        self.total = 0
        self.max_events_in_window = 0
        self.significant_burst_count = 0
        self.significant_bursts.clear()
        self._events.clear()
        self._active = False

    def add(self, uptime_ms: int | None, line_no: int) -> None:
        if uptime_ms is None:
            return
        self.total += 1
        self._events.append((uptime_ms, line_no))
        while (
            self._events
            and uptime_ms - self._events[0][0] >= self.window_s * 1000
        ):
            self._events.popleft()
        count = len(self._events)
        if count > self.max_events_in_window:
            self.max_events_in_window = count
        if count >= self.threshold and not self._active:
            self._active = True
            self.significant_burst_count += 1
            add_sample(
                self.significant_bursts,
                {
                    "event_type": self.event_type,
                    "start_uptime_ms": self._events[0][0],
                    "end_uptime_ms": uptime_ms,
                    "count": count,
                    "line": line_no,
                },
            )
        elif count >= self.threshold and self._active:
            if self.significant_bursts:
                self.significant_bursts[-1].update(
                    {
                        "start_uptime_ms": self._events[0][0],
                        "end_uptime_ms": uptime_ms,
                        "count": count,
                        "line": line_no,
                    }
                )
        elif count < self.threshold:
            self._active = False

    def asdict(self) -> dict[str, Any]:
        return {
            "event_type": self.event_type,
            "total": self.total,
            "max_events_in_window": self.max_events_in_window,
            "window_s": self.window_s,
            "threshold": self.threshold,
            "significant_bursts": self.significant_bursts,
            "significant_burst_count": self.significant_burst_count,
            "sample_limit": EVENT_SAMPLE_LIMIT,
        }


def build_business_outages(
    events: list[tuple[int, int]],
    first_uptime_ms: int | None,
    last_uptime_ms: int | None,
    threshold_s: float,
    total_lines: int,
) -> list[dict[str, Any]]:
    """找出长时间没有成功业务事件的区间。"""
    if (
        first_uptime_ms is None
        or last_uptime_ms is None
        or not events
    ):
        return []

    ordered = sorted(set(events))
    threshold_ms = int(threshold_s * 1000)
    outages: list[dict[str, Any]] = []

    previous_up, previous_line = ordered[0]
    for uptime_ms, line_no in ordered[1:]:
        gap = uptime_ms - previous_up
        if gap >= threshold_ms:
            outages.append(
                {
                    "start_uptime_ms": previous_up,
                    "end_uptime_ms": uptime_ms,
                    "duration_s": round(gap / 1000.0, 3),
                    "start_line": previous_line,
                    "end_line": line_no,
                }
            )
        previous_up, previous_line = uptime_ms, line_no

    tail = last_uptime_ms - previous_up
    if tail >= threshold_ms:
        outages.append(
            {
                "start_uptime_ms": previous_up,
                "end_uptime_ms": last_uptime_ms,
                "duration_s": round(tail / 1000.0, 3),
                "start_line": previous_line,
                "end_line": total_lines,
            }
        )
    return outages


def analyze_log(
    path: str | Path,
    role: str,
    gap_threshold_s: float,
    mac_storm_threshold: int = 20,
    mac_window_s: float = 60.0,
    rf_failure_run_threshold: int = 5,
) -> dict[str, Any]:
    """流式分析单份 ESP 串口日志。"""
    total_lines = 0
    esp_lines = 0
    first_uptime_ms: int | None = None
    last_uptime_ms: int | None = None
    previous_uptime_ms: int | None = None
    uptime_rollbacks = 0
    saw_uptime = False

    counts: Counter[str] = Counter()
    observation_gaps: list[dict[str, Any]] = []
    fatal_events: list[dict[str, Any]] = []
    restart_events: list[dict[str, Any]] = []
    initial_boot_markers: list[dict[str, Any]] = []

    first_stats: dict[str, int] | None = None
    last_stats: dict[str, int] | None = None

    fatal_count = 0
    restart_count = 0
    initial_boot_count = 0
    observation_gap_count = 0
    mac_burst_count = 0
    mac_total = 0
    mac_window: deque[int] = deque()
    mac_max_in_window = 0
    mac_bursts: list[dict[str, Any]] = []
    mac_burst_active = False

    rf_status_counts: Counter[str] = Counter()
    rf_send_failed_total = 0
    rf_send_failures: list[dict[str, Any]] = []
    rf_sustained_runs: list[dict[str, Any]] = []
    rf_send_recoveries: list[dict[str, Any]] = []
    rf_send_unknown_total = 0
    rf_current_failed_run = 0
    rf_max_failed_run = 0
    rf_sustained_run_count = 0
    rf_sustained_active = False
    rf_failure_start: dict[str, Any] | None = None

    node_rejoin_storm = TimedEventStorm(
        "node_rejoin_failure", 60.0, 20
    )
    wifi_mf_storm = TimedEventStorm("wifi_mf_null", 60.0, 20)
    business_events: list[tuple[int, int]] = []
    business_event_segments: list[list[tuple[int, int]]] = []
    boot_segment_bounds: list[tuple[int, int]] = []
    accumulated_duration_ms = 0

    def reset_rf_failure_run() -> None:
        """重启或遇到未知结果时，只断开当前连续失败段；累计历史保留。"""
        nonlocal rf_current_failed_run, rf_sustained_active, rf_failure_start
        rf_current_failed_run = 0
        rf_sustained_active = False
        rf_failure_start = None

    with Path(path).open("r", encoding="utf-8", errors="ignore") as handle:
        for raw_line in handle:
            total_lines += 1
            line_no = total_lines
            line = ANSI_RE.sub("", raw_line).rstrip("\r\n")

            match = ESP_RE.match(line)
            if match:
                esp_lines += 1
                uptime_ms: int | None = int(match.group("uptime"))
                text = match.group("text")
                body = text
            else:
                uptime_ms = None
                body = line

            # boot/rst 行通常没有 uptime 前缀。观察开始后的此类行视为重启。
            if saw_uptime and any(pattern.search(body) for pattern in RESTART_PATTERNS):
                restart_count += 1
                reset_rf_failure_run()
                add_sample(restart_events, _event(line_no, uptime_ms, body))
            elif not saw_uptime and any(
                pattern.search(body) for pattern in RESTART_PATTERNS
            ):
                initial_boot_count += 1
                add_sample(initial_boot_markers, _event(line_no, uptime_ms, body))

            if any(pattern.search(body) for pattern in FATAL_PATTERNS):
                fatal_count += 1
                add_sample(fatal_events, _event(line_no, uptime_ms, body))

            is_mac_no_buffer = bool(
                match
                and match.group("tag") == "ZB_ESP_MAC"
                and MAC_NO_BUFFER_RE.search(body)
            )

            signal_uptime_ms = (
                uptime_ms if uptime_ms is not None else previous_uptime_ms
            )
            if role == "node" and NODE_REJOIN_FAILURE_RE.search(body):
                counts["rejoin_failures"] += 1
                node_rejoin_storm.add(signal_uptime_ms, line_no)
                if not match:
                    counts["raw_rejoin_failures"] += 1
            if (
                role == "gateway"
                and match
                and match.group("tag") == "wifi"
                and WIFI_MF_NULL_RE.search(body)
            ):
                wifi_mf_storm.add(uptime_ms, line_no)

            if not match:
                continue

            if first_uptime_ms is None:
                first_uptime_ms = uptime_ms
            last_uptime_ms = uptime_ms

            if previous_uptime_ms is not None:
                delta_ms = uptime_ms - previous_uptime_ms
                if delta_ms < 0:
                    uptime_rollbacks += 1
                    restart_count += 1
                    # 先封存旧启动段，再按新启动段重置时间轴。
                    if first_uptime_ms is not None:
                        accumulated_duration_ms += (
                            previous_uptime_ms - first_uptime_ms
                        )
                        boot_segment_bounds.append(
                            (first_uptime_ms, previous_uptime_ms)
                        )
                        business_event_segments.append(business_events)
                    # 重启后不能把旧启动段的 MAC/RF 连续段混进新窗口。
                    mac_window.clear()
                    mac_burst_active = False
                    node_rejoin_storm.reset()
                    wifi_mf_storm.reset()
                    business_events = []
                    first_stats = None
                    last_stats = None
                    first_uptime_ms = uptime_ms
                    last_uptime_ms = uptime_ms
                    reset_rf_failure_run()
                    add_sample(
                        restart_events,
                        {
                            "line": line_no,
                            "uptime_ms": uptime_ms,
                            "text": (
                                "uptime 回跳："
                                f"{previous_uptime_ms} ms -> {uptime_ms} ms"
                            ),
                        },
                    )
                elif delta_ms / 1000.0 > gap_threshold_s:
                    observation_gap_count += 1
                    add_sample(
                        observation_gaps,
                        {
                            "line": line_no,
                            "from_ms": previous_uptime_ms,
                            "to_ms": uptime_ms,
                            "gap_s": round(delta_ms / 1000.0, 3),
                        },
                    )

            if role == "node":
                stats_match = REPORT_STATS_RE.search(text)
                if stats_match:
                    stats = parse_report_stats(stats_match.group("body"))
                    if first_stats is None:
                        first_stats = stats
                    last_stats = stats
                if text.startswith("DATA:"):
                    counts["data_samples"] += 1
                if text.startswith("REPORT_SEND:"):
                    counts["report_sends"] += 1
                if text.startswith(("DATA:", "REPORT_SEND:")):
                    business_events.append((uptime_ms, line_no))

            if role == "gateway":
                if TELEMETRY_RE.search(text):
                    counts["telemetry_published"] += 1
                    business_events.append((uptime_ms, line_no))
                if DUPLICATE_RE.search(text):
                    counts["duplicate_suppressed"] += 1
                if CACHED_REPORT_RE.search(text):
                    counts["cached_reports"] += 1
                if RF_SAMPLE_RE.search(text):
                    counts["rf_samples"] += 1
                rf_send_match = RF_SEND_RE.search(text)
                if rf_send_match:
                    counts["rf_commands_sent"] += 1
                    result_match = RF_SEND_RESULT_RE.search(text)
                    send_result = (
                        result_match.group("result")
                        if result_match
                        else "UNKNOWN"
                    )
                    rf_status_counts[send_result] += 1
                    send_event = _event(line_no, uptime_ms, text)

                    if send_result == "ESP_OK":
                        if rf_current_failed_run > 0:
                            add_sample(rf_send_recoveries, send_event)
                        rf_current_failed_run = 0
                        rf_sustained_active = False
                    elif send_result.startswith("ESP_ERR_"):
                        rf_send_failed_total += 1
                        if rf_current_failed_run == 0:
                            rf_failure_start = send_event
                        rf_current_failed_run += 1
                        add_sample(rf_send_failures, send_event)
                        if rf_current_failed_run > rf_max_failed_run:
                            rf_max_failed_run = rf_current_failed_run
                        if (
                            rf_current_failed_run >= rf_failure_run_threshold
                            and not rf_sustained_active
                        ):
                            rf_sustained_active = True
                            rf_sustained_run_count += 1
                            add_sample(
                                rf_sustained_runs,
                                {
                                    "line": line_no,
                                    "start_line": (
                                        rf_failure_start["line"]
                                        if rf_failure_start
                                        else line_no
                                    ),
                                    "start_uptime_ms": (
                                        rf_failure_start["uptime_ms"]
                                        if rf_failure_start
                                        else uptime_ms
                                    ),
                                    "trigger_uptime_ms": uptime_ms,
                                    "length": rf_current_failed_run,
                                    "threshold": rf_failure_run_threshold,
                                },
                            )
                    else:
                        # UNKNOWN 可能来自截断或非标准日志。它需要降级提示，
                        # 但不能计入明确的 ESP_ERR_* 失败次数。
                        rf_send_unknown_total += 1
                        reset_rf_failure_run()
                if RF_ACCEPTED_RE.search(text):
                    counts["rf_reports_accepted"] += 1
                if RF_KEEP_RE.search(text):
                    counts["rf_keeps"] += 1

            if is_mac_no_buffer:
                mac_total += 1
                mac_window.append(uptime_ms)
                while (
                    mac_window
                    and uptime_ms - mac_window[0] >= mac_window_s * 1000
                ):
                    mac_window.popleft()
                window_count = len(mac_window)
                if window_count > mac_max_in_window:
                    mac_max_in_window = window_count
                if window_count >= mac_storm_threshold and not mac_burst_active:
                    mac_burst_active = True
                    mac_burst_count += 1
                    add_sample(
                        mac_bursts,
                        {
                            "start_uptime_ms": mac_window[0],
                            "end_uptime_ms": uptime_ms,
                            "count": window_count,
                            "line": line_no,
                        },
                    )
                elif window_count < mac_storm_threshold:
                    mac_burst_active = False

            previous_uptime_ms = uptime_ms
            saw_uptime = True

    if first_uptime_ms is not None and last_uptime_ms is not None:
        boot_segment_bounds.append((first_uptime_ms, last_uptime_ms))
        business_event_segments.append(business_events)

    business_outages: list[dict[str, Any]] = []
    for segment_index, (events, bounds) in enumerate(
        zip(business_event_segments, boot_segment_bounds)
    ):
        segment_first, segment_last = bounds
        segment_outages = build_business_outages(
            events,
            segment_first,
            segment_last,
            gap_threshold_s,
            total_lines,
        )
        for outage in segment_outages:
            outage["segment_index"] = segment_index
            outage["segment_base_ms"] = segment_first
            business_outages.append(outage)
    business_outage_count = len(business_outages)

    duration_s = round(accumulated_duration_ms / 1000.0, 3)
    if first_uptime_ms is not None and last_uptime_ms is not None:
        duration_s = round(
            (
                accumulated_duration_ms
                + (last_uptime_ms - first_uptime_ms)
            )
            / 1000.0,
            3,
        )

    return {
        "file": str(path),
        "role": role,
        "lines": total_lines,
        "esp_lines": esp_lines,
        "first_uptime_ms": first_uptime_ms,
        "last_uptime_ms": last_uptime_ms,
        "duration_s": duration_s,
        "counts": dict(counts),
        "first_report_stats": first_stats,
        "last_report_stats": last_stats,
        "observation_gaps": observation_gaps,
        "observation_gap_count": observation_gap_count,
        "business_events": business_events,
        "business_event_segments": business_event_segments,
        "boot_segments": [
            {
                "segment_index": index,
                "first_uptime_ms": first,
                "last_uptime_ms": last,
            }
            for index, (first, last) in enumerate(boot_segment_bounds)
        ],
        "business_outages": business_outages,
        "business_outage_count": business_outage_count,
        "node_rejoin_storm": node_rejoin_storm.asdict(),
        "wifi_mf_storm": wifi_mf_storm.asdict(),
        "fatal_events": fatal_events,
        "fatal_event_count": fatal_count,
        "restart_events": restart_events,
        "restart_event_count": restart_count,
        "initial_boot_markers": initial_boot_markers,
        "initial_boot_marker_count": initial_boot_count,
        "uptime_rollbacks": uptime_rollbacks,
        "mac_no_buffer": {
            "total": mac_total,
            "max_events_in_window": mac_max_in_window,
            "window_s": mac_window_s,
            "storm_threshold": mac_storm_threshold,
            "significant_bursts": mac_bursts,
            "significant_burst_count": mac_burst_count,
            "sample_limit": EVENT_SAMPLE_LIMIT,
        },
        "rf_send": {
            "total": counts.get("rf_commands_sent", 0),
            "ok": rf_status_counts.get("ESP_OK", 0),
            "failed": rf_send_failed_total,
            "unknown": rf_send_unknown_total,
            "status_counts": dict(rf_status_counts),
            "failures": rf_send_failures,
            "failure_count": rf_send_failed_total,
            "unknown_count": rf_send_unknown_total,
            "max_consecutive_failures": rf_max_failed_run,
            "sustained_run_threshold": rf_failure_run_threshold,
            "sustained_run_count": rf_sustained_run_count,
            "sustained_runs": rf_sustained_runs,
            "recoveries": rf_send_recoveries,
            "sample_limit": EVENT_SAMPLE_LIMIT,
        },
    }


def _role_events(report: dict[str, Any]) -> list[str]:
    role = "节点" if report["role"] == "node" else "网关"
    reasons: list[str] = []
    for event in report["fatal_events"][:10]:
        reasons.append(
            f"{role}第 {event['line']} 行出现严重错误：{event['text']}"
        )
    for event in report["restart_events"][:10]:
        reasons.append(
            f"{role}第 {event['line']} 行检测到重启：{event['text']}"
        )
    mac = report["mac_no_buffer"]
    if mac["max_events_in_window"] >= mac["storm_threshold"]:
        reasons.append(
            f"{role}出现 MAC no-buffer 风暴：{mac['window_s']} 秒窗口峰值 "
            f"{mac['max_events_in_window']}，阈值 {mac['storm_threshold']}"
        )

    rf_send = report.get("rf_send")
    if (
        rf_send
        and rf_send["max_consecutive_failures"]
        >= rf_send["sustained_run_threshold"]
    ):
        reasons.append(
            f"{role}连续 {rf_send['max_consecutive_failures']} 次 RF 下发失败"
            f"（阈值 {rf_send['sustained_run_threshold']}），下行 APS 链路持续不可用"
        )
    return reasons


def _interval_overlap(
    first: dict[str, Any], second: dict[str, Any]
) -> tuple[int, int] | None:
    start = max(first["start_uptime_ms"], second["start_uptime_ms"])
    end = min(first["end_uptime_ms"], second["end_uptime_ms"])
    if end > start:
        return start, end
    return None


def _storm_overlaps_blackouts(
    report: dict[str, Any],
    storm_key: str,
    blackouts: list[dict[str, Any]],
    base_key: str,
) -> list[dict[str, Any]]:
    """检查错误风暴是否落在双端业务黑障对应一侧。"""
    overlaps: list[dict[str, Any]] = []
    storm = report[storm_key]
    for blackout in blackouts:
        base = blackout[base_key]
        blackout_absolute = {
            "start_uptime_ms": base + blackout["start_relative_ms"],
            "end_uptime_ms": base + blackout["end_relative_ms"],
        }
        for burst in storm["significant_bursts"]:
            start = max(
                blackout_absolute["start_uptime_ms"],
                burst["start_uptime_ms"],
            )
            end = min(
                blackout_absolute["end_uptime_ms"],
                burst["end_uptime_ms"],
            )
            # 无 uptime 的 ZBOSS 批次可能只形成一个时间点；
            # 该点落入黑障区间即可作为佐证，不把零长度本身当持续时间证据。
            if start > end:
                continue
            overlaps.append(
                {
                    "segment_index": blackout["segment_index"],
                    "start_uptime_ms": start,
                    "end_uptime_ms": end,
                    "duration_s": round(
                        max(0, end - start) / 1000.0, 3
                    ),
                    "storm_total": storm["total"],
                    "storm_window_peak": storm[
                        "max_events_in_window"
                    ],
                    "threshold": storm["threshold"],
                }
            )
    return overlaps


def _paired_business_blackouts(
    node: dict[str, Any], gateway: dict[str, Any]
) -> list[dict[str, Any]]:
    blackouts: list[dict[str, Any]] = []
    for node_outage in node["business_outages"]:
        for gateway_outage in gateway["business_outages"]:
            if (
                node_outage["segment_index"]
                != gateway_outage["segment_index"]
            ):
                continue
            node_base = node_outage["segment_base_ms"]
            gateway_base = gateway_outage["segment_base_ms"]
            node_rel = {
                "start_uptime_ms": (
                    node_outage["start_uptime_ms"] - node_base
                ),
                "end_uptime_ms": (
                    node_outage["end_uptime_ms"] - node_base
                ),
            }
            gateway_rel = {
                "start_uptime_ms": (
                    gateway_outage["start_uptime_ms"] - gateway_base
                ),
                "end_uptime_ms": (
                    gateway_outage["end_uptime_ms"] - gateway_base
                ),
            }
            match = _interval_overlap(node_rel, gateway_rel)
            if match is None:
                continue
            start, end = match
            blackouts.append(
                {
                    "segment_index": node_outage["segment_index"],
                    "start_relative_ms": start,
                    "end_relative_ms": end,
                    "duration_s": round((end - start) / 1000.0, 3),
                    "node_start_line": node_outage["start_line"],
                    "gateway_start_line": gateway_outage[
                        "start_line"
                    ],
                    "node_segment_base_ms": node_base,
                    "gateway_segment_base_ms": gateway_base,
                }
            )
    return blackouts


def analyze_pair(
    node_log: str | Path,
    gateway_log: str | Path,
    target_duration_s: float = 28800,
    gap_threshold_s: float = 300,
    mac_storm_threshold: int = 20,
    rf_failure_run_threshold: int = 5,
) -> dict[str, Any]:
    """分析双日志并给出汇总判定。"""
    node = analyze_log(
        node_log,
        "node",
        gap_threshold_s=gap_threshold_s,
        mac_storm_threshold=mac_storm_threshold,
        rf_failure_run_threshold=rf_failure_run_threshold,
    )
    gateway = analyze_log(
        gateway_log,
        "gateway",
        gap_threshold_s=gap_threshold_s,
        mac_storm_threshold=mac_storm_threshold,
        rf_failure_run_threshold=rf_failure_run_threshold,
    )

    first_stats = node.get("first_report_stats") or {}
    last_stats = node.get("last_report_stats") or {}
    deltas = stat_deltas(first_stats, last_stats)
    node["deltas"] = deltas

    # 节点是本次业务主链路，整体覆盖时长以节点为准；节点缺失时再看网关。
    duration_s = node["duration_s"] or gateway["duration_s"]

    failure_reasons = _role_events(node) + _role_events(gateway)

    degraded_reasons: list[str] = []
    for key in DEGRADED_STAT_KEYS:
        value = deltas.get(key, 0)
        if value > 0:
            degraded_reasons.append(f"节点 REPORT_STATS `{key}` 增加 {value}")

    for report in (node, gateway):
        role = "节点" if report["role"] == "node" else "网关"
        for gap in report["observation_gaps"]:
            degraded_reasons.append(
                f"{role}第 {gap['line']} 行附近观察缺口 {gap['gap_s']} 秒"
            )
        mac = report["mac_no_buffer"]
        if mac["total"] > 0 and mac["max_events_in_window"] < mac["storm_threshold"]:
            degraded_reasons.append(
                f"{role}出现 {mac['total']} 次 MAC no-buffer，但未达到风暴阈值"
            )
        rf_send = report.get("rf_send")
        if rf_send and rf_send["failed"] > 0:
            failed_statuses = ", ".join(
                f"{status}×{count}"
                for status, count in sorted(rf_send["status_counts"].items())
                if status.startswith("ESP_ERR_")
            )
            degraded_reasons.append(
                f"{role}RF 下发明确失败 {rf_send['failed']} 次"
                f"（{failed_statuses}），最长连续 "
                f"{rf_send['max_consecutive_failures']} 次"
            )
        if rf_send and rf_send["unknown"] > 0:
            degraded_reasons.append(
                f"{role}有 {rf_send['unknown']} 次 RF 下发结果无法解析"
            )

    paired_business_blackouts = _paired_business_blackouts(
        node, gateway
    )
    node_rejoin_outages = _storm_overlaps_blackouts(
        node,
        "node_rejoin_storm",
        paired_business_blackouts,
        "node_segment_base_ms",
    )
    wifi_mf_outages = _storm_overlaps_blackouts(
        gateway,
        "wifi_mf_storm",
        paired_business_blackouts,
        "gateway_segment_base_ms",
    )

    if node_rejoin_outages:
        failure_reasons.append(
            "节点 rejoin failure 风暴叠加全链路业务中断：最长重叠 "
            f"{max(x['duration_s'] for x in node_rejoin_outages):.1f} 秒，"
            "节点无法重新接入网关"
        )
    if wifi_mf_outages:
        failure_reasons.append(
            "网关 `wifi:m f null` 风暴叠加全链路业务中断：最长重叠 "
            f"{max(x['duration_s'] for x in wifi_mf_outages):.1f} 秒"
        )

    if duration_s >= 60 and gateway["counts"].get("telemetry_published", 0) == 0:
        degraded_reasons.append("网关超过 60 秒未发布 MQTT 遥测")

    paired_segments = {
        item["segment_index"] for item in paired_business_blackouts
    }
    node_single_outages = [
        outage
        for outage in node["business_outages"]
        if outage["segment_index"] not in paired_segments
    ]
    gateway_single_outages = [
        outage
        for outage in gateway["business_outages"]
        if outage["segment_index"] not in paired_segments
    ]
    if node_single_outages:
        degraded_reasons.append(
            "节点上报出现中断，但网关 MQTT 未在同一启动段同时中断；最长 "
            f"{max(x['duration_s'] for x in node_single_outages):.1f} 秒"
        )
    if gateway_single_outages:
        degraded_reasons.append(
            "网关 MQTT 出现中断，但节点上报未在同一启动段同时中断；最长 "
            f"{max(x['duration_s'] for x in gateway_single_outages):.1f} 秒"
        )

    if paired_business_blackouts and not (
        node_rejoin_outages or wifi_mf_outages
    ):
        degraded_reasons.append(
            "全链路业务黑障：节点上报和网关 MQTT 同时中断，最长 "
            f"{max(x['duration_s'] for x in paired_business_blackouts):.1f} 秒"
        )

    raw_rejoin = node["counts"].get("raw_rejoin_failures", 0)
    if raw_rejoin >= 20:
        degraded_reasons.append(
            f"日志中存在 {raw_rejoin} 条无时间戳 rejoin failure，"
            "无法仅凭串口行确认持续时长"
        )

    if (
        node["node_rejoin_storm"]["significant_burst_count"] > 0
        and not node_rejoin_outages
    ):
        degraded_reasons.append(
            "节点出现 rejoin failure 风暴，但未与全链路业务中断重叠"
        )
    if (
        gateway["wifi_mf_storm"]["significant_burst_count"] > 0
        and not wifi_mf_outages
    ):
        degraded_reasons.append(
            "网关出现 `wifi:m f null` 风暴，但未与全链路业务中断重叠"
        )

    incomplete_reasons: list[str] = []
    if duration_s < target_duration_s:
        remaining = max(0.0, target_duration_s - duration_s)
        incomplete_reasons.append(
            f"目标时长 {target_duration_s:g} 秒未满足：当前覆盖 "
            f"{duration_s:.1f} 秒，还差 {remaining:.1f} 秒"
        )
    if not first_stats:
        incomplete_reasons.append("节点日志中尚未出现 REPORT_STATS，无法核对累计统计")
    if gateway["counts"].get("telemetry_published", 0) == 0:
        incomplete_reasons.append("网关日志中尚未记录 MQTT 遥测发布")
    if node["lines"] == 0:
        incomplete_reasons.append("节点日志为空")
    if gateway["lines"] == 0:
        incomplete_reasons.append("网关日志为空")

    if failure_reasons:
        verdict = "FAIL"
    elif degraded_reasons:
        verdict = "DEGRADED"
    elif incomplete_reasons:
        verdict = "INCOMPLETE"
    else:
        verdict = "PASS"

    return {
        "verdict": verdict,
        "duration_s": duration_s,
        "target_duration_s": target_duration_s,
        "gap_threshold_s": gap_threshold_s,
        "failure_reasons": failure_reasons,
        "degraded_reasons": degraded_reasons,
        "incomplete_reasons": incomplete_reasons,
        "node": node,
        "gateway": gateway,
        "paired_business_blackouts": paired_business_blackouts,
    }


def _format_reasons(items: list[str]) -> list[str]:
    if not items:
        return ["- 无"]
    return [f"- {item}" for item in items]


def format_markdown(result: dict[str, Any]) -> str:
    node = result["node"]
    gateway = result["gateway"]
    first_stats = node.get("first_report_stats") or {}
    last_stats = node.get("last_report_stats") or {}
    deltas = node.get("deltas") or {}

    lines = [
        "# 长稳定性测试日志分析",
        "",
        f"**结论：{result['verdict']}**",
        "",
        "## 总览",
        "",
        f"- 日志覆盖时长：{result['duration_s']:.1f} 秒",
        f"- 目标时长：{result['target_duration_s']:g} 秒",
        f"- 观察缺口阈值：{result['gap_threshold_s']:g} 秒",
        f"- 节点日志：{node['lines']} 行，ESP 日志 {node['esp_lines']} 行",
        f"- 网关日志：{gateway['lines']} 行，ESP 日志 {gateway['esp_lines']} 行",
        "",
        "### 失败原因",
        "",
        *_format_reasons(result["failure_reasons"]),
        "",
        "### 降级原因",
        "",
        *_format_reasons(result["degraded_reasons"]),
        "",
        "### 未完成原因",
        "",
        *_format_reasons(result["incomplete_reasons"]),
        "",
        "## 节点 REPORT_STATS",
        "",
        "| 指标 | 首轮 | 末轮 | 增量 |",
        "| --- | ---: | ---: | ---: |",
    ]

    def outage_summary(label: str, report: dict[str, Any]) -> list[str]:
        out = [
            f"- {label}：{report['business_outage_count']} 段",
        ]
        for outage in report["business_outages"][:5]:
            out.append(
                f"  - 第 {outage['start_line']}–{outage['end_line']} 行："
                f"{outage['duration_s']:.1f} 秒无成功业务事件"
            )
        return out

    lines.extend(
        [
            "",
            "## 业务连续性",
            "",
            *outage_summary("节点上报黑障", node),
            *outage_summary("网关 MQTT 黑障", gateway),
            f"- 双端重叠黑障：{len(result['paired_business_blackouts'])} 段",
        ]
    )
    for blackout in result["paired_business_blackouts"][:5]:
        lines.append(
            "  - "
            f"节点第 {blackout['node_start_line']} 行、网关第 "
            f"{blackout['gateway_start_line']} 行起，重叠 "
            f"{blackout['duration_s']:.1f} 秒"
        )

    node_rejoin = node["node_rejoin_storm"]
    wifi_mf = gateway["wifi_mf_storm"]
    lines.extend(
        [
            "",
            "## 错误风暴",
            "",
            "- 节点 rejoin failure："
            f"{node_rejoin['total']} 次，60 秒窗口峰值 "
            f"{node_rejoin['max_events_in_window']}，显著突发 "
            f"{node_rejoin['significant_burst_count']} 个",
            "- 网关 wifi:m f null："
            f"{wifi_mf['total']} 次，60 秒窗口峰值 "
            f"{wifi_mf['max_events_in_window']}，显著突发 "
            f"{wifi_mf['significant_burst_count']} 个",
        ]
    )

    keys = tuple(dict.fromkeys((*NODE_STAT_KEYS, *first_stats, *last_stats)))
    for key in keys:
        lines.append(
            f"| `{key}` | {first_stats.get(key, '-')} | "
            f"{last_stats.get(key, '-')} | {deltas.get(key, '-')} |"
        )

    node_counts = node["counts"]
    gateway_counts = gateway["counts"]
    gateway_rf_send = gateway["rf_send"]
    lines.extend(
        [
            "",
            "## 业务计数",
            "",
            f"- 节点 DATA：{node_counts.get('data_samples', 0)}；"
            f"REPORT_SEND：{node_counts.get('report_sends', 0)}。",
            f"- 网关 MQTT 遥测发布：{gateway_counts.get('telemetry_published', 0)}；"
            f"重复抑制：{gateway_counts.get('duplicate_suppressed', 0)}；"
            f"缓存 report：{gateway_counts.get('cached_reports', 0)}。",
            f"- 网关 RF samples：{gateway_counts.get('rf_samples', 0)}；"
            f"RF 下发：{gateway_counts.get('rf_commands_sent', 0)}"
            f"（OK {gateway_rf_send['ok']} / 明确失败 "
            f"{gateway_rf_send['failed']} / 未知 "
            f"{gateway_rf_send['unknown']}，最长连续 "
            f"{gateway_rf_send['max_consecutive_failures']}）；"
            f"RF accepted：{gateway_counts.get('rf_reports_accepted', 0)}；"
            f"keep：{gateway_counts.get('rf_keeps', 0)}。",
            "",
            "## 观察缺口",
            "",
            f"- 节点：{node['observation_gap_count']} 个（报告最多展示 "
            f"{len(node['observation_gaps'])} 条样本）",
            f"- 网关：{gateway['observation_gap_count']} 个（报告最多展示 "
            f"{len(gateway['observation_gaps'])} 条样本）",
        ]
    )

    for report in (node, gateway):
        role = "节点" if report["role"] == "node" else "网关"
        for gap in report["observation_gaps"][:10]:
            lines.append(
                f"  - {role}第 {gap['line']} 行：{gap['from_ms']} -> "
                f"{gap['to_ms']}，间隔 {gap['gap_s']} 秒"
            )

    lines.extend(
        [
            "",
            "## 重启与严重事件",
            "",
            f"- 节点 uptime 回跳：{node['uptime_rollbacks']}；"
            f"严重事件：{node['fatal_event_count']}；"
            f"重启事件：{node['restart_event_count']}；"
            f"采集前 boot 标记：{node['initial_boot_marker_count']}。",
            f"- 网关 uptime 回跳：{gateway['uptime_rollbacks']}；"
            f"严重事件：{gateway['fatal_event_count']}；"
            f"重启事件：{gateway['restart_event_count']}；"
            f"采集前 boot 标记：{gateway['initial_boot_marker_count']}。",
        ]
    )

    for report in (node, gateway):
        role = "节点" if report["role"] == "node" else "网关"
        for event in (*report["fatal_events"], *report["restart_events"])[:10]:
            lines.append(
                f"  - {role}第 {event['line']} 行：{event['text']}"
            )

    rf_status_bits = ", ".join(
        f"{status}×{count}"
        for status, count in sorted(gateway_rf_send["status_counts"].items())
    ) or "无"
    lines.extend(
        [
            "",
            "## 网关 RF 下发结果",
            "",
            f"- 总数：{gateway_rf_send['total']}；成功：{gateway_rf_send['ok']}；"
            f"明确失败：{gateway_rf_send['failed']}；"
            f"未知：{gateway_rf_send['unknown']}。",
            f"- 状态分布：{rf_status_bits}。",
            f"- 最长连续失败：{gateway_rf_send['max_consecutive_failures']}；"
            f"持续失败阈值：{gateway_rf_send['sustained_run_threshold']}；"
            f"持续失败段：{gateway_rf_send['sustained_run_count']}。",
        ]
    )
    for event in gateway_rf_send["failures"][:10]:
        lines.append(
            f"  - 第 {event['line']} 行（uptime {event['uptime_ms']} ms）："
            f"{event['text']}"
        )

    for report in (node, gateway):
        role = "节点" if report["role"] == "node" else "网关"
        mac = report["mac_no_buffer"]
        lines.extend(
            [
                "",
                f"## {role} MAC no-buffer",
                "",
                f"- 总数：{mac['total']}",
                f"- {mac['window_s']:g} 秒窗口峰值：{mac['max_events_in_window']}",
                f"- 风暴阈值：{mac['storm_threshold']}",
                f"- 显著突发：{mac['significant_burst_count']} 个（报告最多展示 "
                f"{len(mac['significant_bursts'])} 条样本）",
            ]
        )

    lines.append("")
    return "\n".join(lines)


def positive_float(value: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("必须是正数") from exc
    if number <= 0:
        raise argparse.ArgumentTypeError("必须是正数")
    return number


def positive_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("必须是正整数") from exc
    if number <= 0:
        raise argparse.ArgumentTypeError("必须是正整数")
    return number


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="分析节点/网关双串口长稳定性测试日志"
    )
    parser.add_argument("node_log", type=Path)
    parser.add_argument("gateway_log", type=Path)
    parser.add_argument(
        "--target-duration",
        type=positive_float,
        default=28800,
        help="目标测试时长，默认 28800 秒（8 小时）",
    )
    parser.add_argument(
        "--gap-threshold",
        type=positive_float,
        default=300,
        help="观察缺口和业务黑障阈值，默认 300 秒",
    )
    parser.add_argument(
        "--mac-storm-threshold",
        type=positive_int,
        default=20,
        help="MAC no-buffer 风暴阈值，默认 60 秒内 20 次",
    )
    parser.add_argument(
        "--rf-failure-run-threshold",
        type=positive_int,
        default=5,
        help="连续 RF 下发失败阈值，默认 5 次",
    )
    parser.add_argument("--json", type=Path, help="写入 JSON 结果")
    parser.add_argument("--markdown", type=Path, help="写入 Markdown 报告")
    return parser.parse_args()


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    args = parse_args()
    result = analyze_pair(
        args.node_log,
        args.gateway_log,
        target_duration_s=args.target_duration,
        gap_threshold_s=args.gap_threshold,
        mac_storm_threshold=args.mac_storm_threshold,
        rf_failure_run_threshold=args.rf_failure_run_threshold,
    )

    markdown = format_markdown(result)
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(markdown, encoding="utf-8")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(result, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
    print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())










