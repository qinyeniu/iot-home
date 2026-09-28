from pathlib import Path
import re
from collections import Counter
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
TEST_DIR = ROOT / "backups/long-test-20260928-default-8h"
GW_LOG = TEST_DIR / "gateway.log"
NODE_LOG = TEST_DIR / "node.log"
ANALYSIS = TEST_DIR / "analysis.md"
MORNING = ROOT / "docs/MORNING_REPORT_2026-09-28.md"
HANDOFF = ROOT / "docs/HANDOFF_2026-09-28.md"
ENTRY = ROOT / "docs/HANDOFF/_2026-09-28.md"

CRITICAL_TERMS = [
    "Guru Meditation", "panic", "Panic", "abort()", "Backtrace",
    "watchdog", "Watchdog", "ESP_ERR_NO_MEM", "MQTT disconnected",
    "MQTT write timeout", "Rejoin failure", "clear bus error",
    "OLED refresh failed",
]
WARNING_TERMS = ["parent unavailable", "NLME status indication"]
BOOT_MARKER = "ESP-ROM:esp32c6"
PLANNED_DURATION_SEC = 28800


def after_last_boot(text: str) -> str:
    index = text.rfind(BOOT_MARKER)
    return text[index:] if index >= 0 else text


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def uptimes(text):
    return [int(x) for x in re.findall(r"[IWE] \((\d+)\)", text)]


def count(pattern, text):
    return len(re.findall(pattern, text, re.M))


def term_hits(text, terms):
    return {term: text.count(term) for term in terms if text.count(term) > 0}


def summarize_resources(rows):
    if not rows:
        return {}
    tasks = ("zb", "wifi", "oled", "status", "fwd")
    return {
        "snapshots": len(rows),
        "min_free": min(r["free"] for r in rows),
        "final_free": rows[-1]["free"],
        "min_reported_min": min(r["min"] for r in rows),
        "min_largest_block": min(r["block"] for r in rows),
        "final_largest_block": rows[-1]["block"],
        "reset_reasons": dict(sorted(Counter(r["reset"] for r in rows).items())),
        "min_stack": {
            name: min((r[name] for r in rows if r[name] > 0), default=None)
            for name in tasks
        },
        "final_stack": {name: rows[-1][name] for name in tasks},
    }


def file_span(path: Path):
    try:
        stat = path.stat()
        return stat.st_mtime - stat.st_birthtime
    except (OSError, AttributeError):
        return None


def parse_resources(gw):
    rows = []
    pat = re.compile(
        r"Resource: free=(\d+) min=(\d+) block=(\d+) reset=(\S+) "
        r"stack_free\[zb=(\d+) wifi=(\d+) oled=(\d+) status=(\d+) fwd=(\d+)\]"
    )
    for line in gw.splitlines():
        m = pat.search(line)
        if m:
            rows.append({
                "free": int(m.group(1)),
                "min": int(m.group(2)),
                "block": int(m.group(3)),
                "reset": m.group(4),
                "zb": int(m.group(5)),
                "wifi": int(m.group(6)),
                "oled": int(m.group(7)),
                "status": int(m.group(8)),
                "fwd": int(m.group(9)),
            })
    return rows


def parse_node_stats(node):
    rows = []
    pat = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([0-9]+(?:/[0-9]+)?)")
    for line in node.splitlines():
        if "REPORT_STATS:" not in line:
            continue
        values = {}
        for key, value in pat.findall(line):
            values[key] = int(value.split("/")[0])
        if values:
            rows.append(values)
    return rows


def replace_section(path: Path, header: str, body: str, level: str = "##"):
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"\n{re.escape(header)}\n.*?(?=\n{level} |\Z)", re.S
    )
    section = f"\n{header}\n\n{body.rstrip()}\n"
    text = pattern.sub(section, text)
    if header not in text:
        text = text.rstrip() + section
    path.write_text(text, encoding="utf-8")


def insert_after_title(path: Path, header: str, body: str):
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"\n{re.escape(header)}\n.*?(?=\n## |\Z)", re.S
    )
    section = f"\n{header}\n\n{body.rstrip()}\n"
    text = pattern.sub(section, text)
    if header not in text:
        lines = text.splitlines(keepends=True)
        text = "".join(lines[:1]) + section + "".join(lines[1:])
    path.write_text(text, encoding="utf-8")


def main():
    raw_gw = read(GW_LOG)
    raw_node = read(NODE_LOG)
    gw_boot_count = raw_gw.count(BOOT_MARKER)
    node_boot_count = raw_node.count(BOOT_MARKER)
    gw = after_last_boot(raw_gw)
    node = after_last_boot(raw_node)
    gw_up = uptimes(gw)
    node_up = uptimes(node)
    gw_file_span = file_span(GW_LOG)
    node_file_span = file_span(NODE_LOG)

    gw_counts = {
        "RF sample": count(r"RF sample", gw),
        "cached report": count(r"Zigbee: cached report", gw),
        "MQTT telemetry": count(r"MQTT -> nodes/.*/telemetry", gw),
        "MQTT duplicate suppressed": count(r"MQTT duplicate suppressed", gw),
        "Resource snapshots": count(r"iot-gw: Resource:", gw),
        "RF power query accepted": count(r"RF power: report accepted", gw),
        "RF power keep": count(r"RF power: keep", gw),
        "APS confirm OK": count(r"APS data confirm: status=0", gw),
        "NLME status": count(r"NLME status indication", gw),
        "boot markers": gw_boot_count,
        "OLED refresh failed": count(r"OLED refresh failed", gw),
        "OLED recovered by reset": count(r"OLED recovered by", gw),
        "OLED refresh recovered": count(r"OLED refresh recovered", gw),
        "clear bus error": count(r"clear bus error", gw),
        "MQTT disconnected events": count(r"MQTT disconnected", gw),
        "MQTT write timeout events": count(r"MQTT write timeout", gw),
        "Rejoin failure events": count(r"Rejoin failure", gw),
    }
    node_counts = {
        "DATA": count(r"zb-sensor: DATA:", node),
        "REPORT_SEND": count(r"REPORT_SEND:", node),
        "REPORT_STATS": count(r"REPORT_STATS:", node),
        "parent unavailable": count(r"parent unavailable event=", node),
        "parent recovered": count(r"parent unavailable resolved", node),
        "boot markers": node_boot_count,
    }

    gw_critical = term_hits(gw, CRITICAL_TERMS)
    node_critical = term_hits(node, CRITICAL_TERMS)
    gw_warnings = (
        {"NLME status indication": gw_counts["NLME status"]}
        if gw_counts["NLME status"] else {}
    )
    node_warnings = (
        {"parent unavailable event": node_counts["parent unavailable"]}
        if node_counts["parent unavailable"] else {}
    )
    resources = parse_resources(gw)
    node_stats = parse_node_stats(node)
    resource_summary = summarize_resources(resources)

    observed_ms = 0
    if gw_up:
        observed_ms = max(observed_ms, max(gw_up) - min(gw_up))
    if node_up:
        observed_ms = max(observed_ms, max(node_up) - min(node_up))

    expected_minimum = {
        "RF sample": 700,
        "cached report": 700,
        "MQTT telemetry": 200,
        "Resource snapshots": 150,
    }
    low_counts = {
        name: gw_counts[name]
        for name, minimum in expected_minimum.items()
        if gw_counts[name] < minimum
    }

    unexpected_boots = {}
    if gw_boot_count != 1:
        unexpected_boots["gateway boot markers"] = gw_boot_count
    if node_boot_count != 1:
        unexpected_boots["node boot markers"] = node_boot_count

    if gw_critical or node_critical or unexpected_boots or low_counts:
        verdict = "FAIL/WARN"
    elif gw_warnings or node_warnings:
        verdict = "PASS_WITH_WARNINGS"
    else:
        verdict = "PASS"

    final_resource = resources[-1] if resources else None
    min_heap = min((r["min"] for r in resources), default=None)
    stats_delta = {}
    if node_stats:
        first, last = node_stats[0], node_stats[-1]
        stats_delta = {
            key: last.get(key, 0) - first.get(key, 0)
            for key in last
            if key in first
        }

    lines = [
        "# 2026-09-28 默认 OFF 8 小时夜间长测分析",
        "",
        f"生成时间：{datetime.now().astimezone().strftime('%Y-%m-%d %H:%M:%S %z')}",
        "",
        f"结论：**{verdict}**",
        "",
        "## 运行窗口",
        "",
        f"- 网关 uptime：{min(gw_up) if gw_up else 'N/A'} → {max(gw_up) if gw_up else 'N/A'} ms",
        f"- 节点 uptime：{min(node_up) if node_up else 'N/A'} → {max(node_up) if node_up else 'N/A'} ms",
        f"- 网关实际文件采集时长：{gw_file_span:.1f} 秒" if gw_file_span is not None else "- 网关实际文件采集时长：N/A",
        f"- 节点实际文件采集时长：{node_file_span:.1f} 秒" if node_file_span is not None else "- 节点实际文件采集时长：N/A",
        f"- 计划时长：{PLANNED_DURATION_SEC} 秒 / {PLANNED_DURATION_SEC / 3600:.0f} 小时",
        f"- 可观测跨度：约 {observed_ms / 1000:.1f} 秒 / {observed_ms / 3600000:.2f} 小时",
        "",
        "## 网关计数",
        "",
    ]
    lines += [f"- {name}: {value}" for name, value in gw_counts.items()]
    lines += ["", "## 节点计数", ""]
    lines += [f"- {name}: {value}" for name, value in node_counts.items()]
    if stats_delta:
        lines += ["", "## 节点 REPORT_STATS 增量", "", "```text"]
        for name, value in stats_delta.items():
            lines.append(f"{name}: {value:+d}" if name != "probes" else f"{name}: {value:+d}")
        lines += ["```"]
    if final_resource:
        lines += [
            "", "## 最终资源快照", "", "```text",
            f"free={final_resource['free']}",
            f"min={final_resource['min']}",
            f"largest_block={final_resource['block']}",
            f"stack_free[zb={final_resource['zb']} wifi={final_resource['wifi']} "
            f"oled={final_resource['oled']} status={final_resource['status']} fwd={final_resource['fwd']}]",
            f"历史最低堆（资源日志）={min_heap}",
            "```",
        ]
    if resource_summary:
        lines += [
            "", "## 全部资源快照统计", "", "```text",
            f"snapshots={resource_summary['snapshots']}",
            f"free heap: final={resource_summary['final_free']} min={resource_summary['min_free']}",
            f"reported min heap: {resource_summary['min_reported_min']}",
            f"largest block: final={resource_summary['final_largest_block']} min={resource_summary['min_largest_block']}",
            f"reset_reasons={resource_summary['reset_reasons']}",
            "min_stack_free["
            f"zb={resource_summary['min_stack']['zb'] if resource_summary['min_stack']['zb'] is not None else 'N/A'} "
            f"wifi={resource_summary['min_stack']['wifi'] if resource_summary['min_stack']['wifi'] is not None else 'N/A'} "
            f"oled={resource_summary['min_stack']['oled'] if resource_summary['min_stack']['oled'] is not None else 'N/A'} "
            f"status={resource_summary['min_stack']['status'] if resource_summary['min_stack']['status'] is not None else 'N/A'} "
            f"fwd={resource_summary['min_stack']['fwd'] if resource_summary['min_stack']['fwd'] is not None else 'N/A'}]",
            "final_stack_free["
            f"zb={resource_summary['final_stack']['zb']} "
            f"wifi={resource_summary['final_stack']['wifi']} "
            f"oled={resource_summary['final_stack']['oled']} "
            f"status={resource_summary['final_stack']['status']} "
            f"fwd={resource_summary['final_stack']['fwd']}]",
            "```",
        ]
    lines += ["", "## 异常/警告", ""]
    lines.append(f"- 网关 critical: {gw_critical or '无'}")
    lines.append(f"- 节点 critical: {node_critical or '无'}")
    lines.append(f"- 启动标记/意外重启: {unexpected_boots or '无'}")
    lines.append(f"- 网关 warning: {gw_warnings or '无'}")
    lines.append(f"- 节点 warning: {node_warnings or '无'}")
    lines.append(f"- 计数不足: {low_counts or '无'}")
    lines += [
        "", "## 判定依据", "",
        "- 无 critical、计数达到最低预期时为 PASS；",
        "- 仅有父链路/NLME 瞬时提示但业务连续时为 PASS_WITH_WARNINGS；",
        "- 出现 panic/watchdog/内存/MQTT/I2C/OLED 异常或样本显著不足时为 FAIL/WARN。",
    ]
    ANALYSIS.write_text("\n".join(lines) + "\n", encoding="utf-8")

    summary = f"""自动分析已完成，结论：**{verdict}**。

- 可观测跨度：约 `{observed_ms / 3600000:.2f}` 小时
- RF sample：`{gw_counts['RF sample']}`
- cached report：`{gw_counts['cached report']}`
- MQTT telemetry：`{gw_counts['MQTT telemetry']}`
- Resource snapshots：`{gw_counts['Resource snapshots']}`
- 最低 free heap：`{resource_summary.get('min_free', 'N/A')}`
- OLED refresh failed：`{gw_counts['OLED refresh failed']}`
- 网关 critical：`{gw_critical or '无'}`
- 节点 critical：`{node_critical or '无'}`
- 完整分析：`backups/long-test-20260928-default-8h/analysis.md`
"""
    replace_section(MORNING, "## 长测自动分析结果", summary)
    replace_section(HANDOFF, "## 16. 夜间长测自动分析结果", summary)
    insert_after_title(ENTRY, "## 长测自动分析结果", summary)
    print(summary)


if __name__ == "__main__":
    main()
