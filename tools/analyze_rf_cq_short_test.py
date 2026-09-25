#!/usr/bin/env python3
"""分析 CQ-2 RF 命令队列短测日志。

只读取日志，不访问设备、不烧录、不改变网络。默认核对：

1. QUERY / SET 0 / SET -10 / AUTO 的顺序；
2. 每条用户命令是否经过 submit -> dequeue；
3. 消费者是否只从单一 rf-dispatch 路径发送；
4. ESP_OK 后是否收到匹配 seq 的加密 REPORT；
5. rf-dispatch 栈高水位是否安全；
6. 测试期间网关是否重启。
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

ESP_RE = re.compile(
    r"^(?P<level>[IWE]) \((?P<uptime>\d+)\)\s+"
    r"(?P<tag>[^:]+):\s*(?P<text>.*)$"
)
SUBMIT_RE = re.compile(
    r"submit id=(?P<id>\d+) source=(?P<source>\d+) cmd=(?P<cmd>\d+)"
    r"(?: queued=(?P<queued>\d+))?"
)
DEQUEUE_RE = re.compile(
    r"dequeue id=(?P<id>\d+) source=(?P<source>\d+) cmd=(?P<cmd>\d+)"
)
STACK_RE = re.compile(
    r"stack_hwm id=(?P<id>\d+) bytes=(?P<bytes>\d+)"
)
EXPIRED_RE = re.compile(r"drop expired command id=(?P<id>\d+)")
SEND_RE = re.compile(
    r"RF power -> node=(?P<node>0x[0-9a-f]+) cmd=(?P<cmd>\d+) "
    r"power=(?P<power>-?\d+) mode=(?P<mode>auto|manual) "
    r"seq=(?P<seq>\d+):\s+(?P<result>ESP_[A-Z0-9_]+)"
)
ACCEPT_RE = re.compile(
    r"RF power: report accepted .*? to (?P<mode>auto|manual)/"
    r"(?P<power>-?\d+) dBm seq=(?P<seq>\d+)"
)
REJECT_RE = re.compile(
    r"RF power: reject (?P<reason>duplicate/old|mismatched report).*?"
    r"seq=(?P<seq>\d+)"
)
CMD_QUERY_RE = re.compile(r"CMD rf: node=(?P<node>0x[0-9a-f]+) query$")
CMD_SET_RE = re.compile(
    r"CMD rf: node=(?P<node>0x[0-9a-f]+) set (?P<power>-?\d+) dBm$"
)
CMD_AUTO_RE = re.compile(
    r"CMD rf: node=(?P<node>0x[0-9a-f]+) automatic$"
)
NODE_RF_RE = re.compile(
    r"RF power cmd=(?P<cmd>\d+) power=(?P<power>-?\d+) "
    r"mode=(?P<mode>0|1) seq=(?P<seq>\d+):\s+(?P<result>ESP_[A-Z0-9_]+)"
)

FATAL_RE = re.compile(
    r"assert failed|\bpanic\b|guru meditation|fatal exception|abort\(\)|"
    r"stack canary",
    re.IGNORECASE,
)
BOOT_RE = re.compile(r"^rst:0x|^ESP-ROM:|boot:0x", re.IGNORECASE)

EXPECTED_STEPS = ("query", 0, -10, "auto")
STACK_WARN_BYTES = 512
STACK_FAIL_BYTES = 256


@dataclass
class Command:
    id: int
    source: int
    cmd: int
    submitted: bool = False
    dequeued: bool = False
    expired: bool = False
    sends: list[tuple[int, str, int, str, int, int, str]] = field(default_factory=list)
    stack_hwm: Optional[int] = None


@dataclass
class Report:
    warnings: list[str] = field(default_factory=list)
    failures: list[str] = field(default_factory=list)
    infos: list[str] = field(default_factory=list)


def step_name(kind: str, power: Optional[int] = None) -> str:
    if kind == "query":
        return "QUERY"
    if kind == "auto":
        return "AUTO"
    return f"SET {power}"


def parse_gateway(path: Path) -> tuple[dict[int, Command], Report, list[str], bool]:
    commands: dict[int, Command] = {}
    report = Report()
    user_steps: list[str] = []
    saw_cq = False
    sends_without_cq = 0
    app_started = False
    current_id: Optional[int] = None
    last_uptime: Optional[int] = None
    boot_count = 0

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            match = ESP_RE.match(line)
            if not match:
                if app_started and BOOT_RE.search(line):
                    boot_count += 1
                    report.failures.append(
                        f"网关疑似第 {boot_count + 1} 次启动：{line[:120]}"
                    )
                continue

            uptime = int(match.group("uptime"))
            text = match.group("text")
            if last_uptime is not None and uptime + 1000 < last_uptime:
                report.failures.append(
                    f"网关 uptime 回跳：{last_uptime} -> {uptime}"
                )
            last_uptime = uptime
            app_started = True

            if FATAL_RE.search(text):
                report.failures.append(f"网关出现致命错误：{text[:160]}")

            submit = SUBMIT_RE.search(text)
            if submit:
                saw_cq = True
                cid = int(submit.group("id"))
                cmd = commands.setdefault(
                    cid,
                    Command(cid, int(submit.group("source")),
                            int(submit.group("cmd"))),
                )
                cmd.submitted = True
                current_id = cid
                continue

            dequeue = DEQUEUE_RE.search(text)
            if dequeue:
                saw_cq = True
                cid = int(dequeue.group("id"))
                cmd = commands.setdefault(
                    cid,
                    Command(cid, int(dequeue.group("source")),
                            int(dequeue.group("cmd"))),
                )
                cmd.dequeued = True
                current_id = cid
                continue

            stack = STACK_RE.search(text)
            if stack:
                cid = int(stack.group("id"))
                hwm = int(stack.group("bytes"))
                commands.setdefault(cid, Command(cid, 0, 0)).stack_hwm = hwm
                current_id = cid
                continue

            expired = EXPIRED_RE.search(text)
            if expired:
                saw_cq = True
                cid = int(expired.group("id"))
                commands.setdefault(cid, Command(cid, 0, 0)).expired = True
                report.warnings.append(f"命令 {cid} 超过 TTL，被消费者丢弃")
                continue

            send = SEND_RE.search(text)
            if send:
                seq = int(send.group("seq"))
                entry = (
                    uptime,
                    send.group("node"),
                    int(send.group("cmd")),
                    send.group("mode"),
                    seq,
                    int(send.group("power")),
                    send.group("result"),
                )
                if current_id is not None:
                    commands.setdefault(current_id, Command(current_id, 0, 0)).sends.append(entry)
                else:
                    sends_without_cq += 1
                continue

            accepted = ACCEPT_RE.search(text)
            if accepted:
                report.infos.append(
                    f"seq={accepted.group('seq')} REPORT accepted "
                    f"{accepted.group('mode')}/{accepted.group('power')} dBm"
                )
                continue

            rejected = REJECT_RE.search(text)
            if rejected:
                report.warnings.append(
                    f"seq={rejected.group('seq')} REPORT 被拒绝："
                    f"{rejected.group('reason')}"
                )
                continue

            if CMD_QUERY_RE.search(text):
                user_steps.append("query")
            elif match_set := CMD_SET_RE.search(text):
                user_steps.append(f"set:{match_set.group('power')}")
            elif CMD_AUTO_RE.search(text):
                user_steps.append("auto")

    if saw_cq and sends_without_cq:
        report.warnings.append(
            f"{sends_without_cq} 次 RF 发送缺少 CQ 命令上下文"
        )

    return commands, report, user_steps, saw_cq


def check_expected_steps(steps: list[str], report: Report) -> None:
    wanted = list(EXPECTED_STEPS)
    found: list[str] = []
    index = 0

    for step in steps:
        if index >= len(wanted):
            break
        expected = wanted[index]
        if expected == "query" and step == "query":
            found.append("QUERY")
            index += 1
        elif expected == "auto" and step == "auto":
            found.append("AUTO")
            index += 1
        elif isinstance(expected, int) and step == f"set:{expected}":
            found.append(f"SET {expected}")
            index += 1

    if index < len(wanted):
        report.warnings.append(
            "操作顺序未完整覆盖 QUERY -> SET 0 -> SET -10 -> AUTO；"
            f"当前只识别：{', '.join(found) if found else '无'}"
        )
    else:
        report.infos.append("操作顺序正确：" + " -> ".join(found))


def check_commands(commands: dict[int, Command], report: Report) -> None:
    accepted_seqs: set[int] = set()
    # report.infos entries are formatted above; extracting seq is simpler than
    # maintaining a second structure because this script is a read-only tool.
    for info in report.infos:
        match = re.search(r"seq=(\d+)", info)
        if match:
            accepted_seqs.add(int(match.group(1)))

    for cid, cmd in sorted(commands.items()):
        label = f"命令 {cid}"
        if cmd.source != 0:
            report.failures.append(f"{label} 不是用户命令，当前消费者不应处理")
        if not cmd.submitted:
            report.warnings.append(f"{label} 未观察到 submit")
        if not cmd.dequeued:
            report.warnings.append(f"{label} 未观察到 dequeue")
        if cmd.stack_hwm is not None:
            if cmd.stack_hwm < STACK_FAIL_BYTES:
                report.failures.append(
                    f"{label} rf-dispatch 栈剩余仅 {cmd.stack_hwm} 字节"
                )
            elif cmd.stack_hwm < STACK_WARN_BYTES:
                report.warnings.append(
                    f"{label} rf-dispatch 栈剩余偏低：{cmd.stack_hwm} 字节"
                )

        for _, _, _, _, _, _, result in cmd.sends:
            if result != "ESP_OK":
                report.warnings.append(f"{label} 本地发送返回 {result}")

        for _, _, _, _, seq, _, result in cmd.sends:
            if result == "ESP_OK" and seq not in accepted_seqs:
                report.warnings.append(
                    f"{label} seq={seq} 已 ESP_OK，但未观察到匹配 REPORT"
                )


def scan_node(path: Path, report: Report) -> None:
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if FATAL_RE.search(line):
                report.failures.append(f"节点出现致命错误：{line.strip()[:160]}")
            match = NODE_RF_RE.search(line)
            if match and match.group("result") != "ESP_OK":
                report.warnings.append(
                    f"节点 seq={match.group('seq')} 私有命令接收/发送返回 "
                    f"{match.group('result')}"
                )


def write_report(
    gateway: Path,
    node: Optional[Path],
    report: Report,
    saw_cq: bool,
    command_count: int,
) -> int:
    print("# RF CQ 短测报告")
    print()
    print(f"- 网关日志：`{gateway}`")
    if node is not None:
        print(f"- 节点日志：`{node}`")
    print(f"- CQ 命令数：{command_count}")
    print()

    if report.infos:
        print("## 通过项")
        for item in report.infos:
            print(f"- {item}")
        print()

    if report.warnings:
        print("## 降级/待确认")
        for item in report.warnings:
            print(f"- {item}")
        print()

    if report.failures:
        print("## 失败项")
        for item in report.failures:
            print(f"- {item}")
        print()

    if report.failures:
        print("结论：**FAIL**")
        return 3
    if not saw_cq:
        print("结论：**LEGACY/INCOMPLETE**（未发现 CQ 队列日志）")
        return 4
    if report.warnings:
        print("结论：**DEGRADED**")
        return 2
    print("结论：**PASS**")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("gateway_log", type=Path)
    parser.add_argument("--node-log", type=Path)
    args = parser.parse_args()

    if not args.gateway_log.is_file():
        print(f"网关日志不存在：{args.gateway_log}", file=sys.stderr)
        return 4
    if args.node_log is not None and not args.node_log.is_file():
        print(f"节点日志不存在：{args.node_log}", file=sys.stderr)
        return 4

    commands, report, user_steps, saw_cq = parse_gateway(args.gateway_log)
    check_expected_steps(user_steps, report)
    if saw_cq:
        check_commands(commands, report)
    if args.node_log is not None:
        scan_node(args.node_log, report)

    return write_report(
        args.gateway_log, args.node_log, report, saw_cq, len(commands)
    )


if __name__ == "__main__":
    raise SystemExit(main())
