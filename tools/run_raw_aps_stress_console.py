"""Stress the gateway raw APS path through the experimental local console."""
from __future__ import annotations

import argparse
import json
import math
import re
import time
from pathlib import Path
from typing import Any

import serial

ACCEPTED_RE = re.compile(rb"LOCAL_RF_ACCEPTED query request_id=([0-9a-f]{16})")
BUSY_RE = re.compile(rb"LOCAL_RF_BUSY")
COMPLETE_RE = re.compile(rb"LOCAL_RF_COMPLETE request_id=([0-9a-f]{16})")
SEND_RE = re.compile(rb"RF power -> .*?: (ESP_OK|ESP_ERR_[A-Z_]+)")
REPORT_RE = re.compile(
    rb"RF power: report accepted from \S+ to (?:manual|auto)/-?\d+ dBm seq=\d+"
)
NO_MEM_RE = re.compile(rb"ESP_ERR_NO_MEM")
CONFIRM_RE = re.compile(rb"APS data confirm: status=(\d+)")
RESET_RE = re.compile(rb"rst:0x|Guru Meditation|abort\(\)|Backtrace:")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("out", type=Path)
    parser.add_argument("--count", type=int, default=40)
    parser.add_argument("--start-delay", type=float, default=10.0)
    parser.add_argument("--complete-timeout", type=float, default=15.0)
    parser.add_argument("--busy-timeout", type=float, default=10.0)
    parser.add_argument("--tail-gap", type=float, default=0.5)
    parser.add_argument("--final-settle", type=float, default=13.0)
    parser.add_argument(
        "--require-confirm-success",
        action="store_true",
        help="require every raw APS confirm status to be zero",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="overwrite an existing log and summary",
    )
    args = parser.parse_args()

    if args.count <= 0:
        parser.error("count must be positive")
    if args.complete_timeout <= 0:
        parser.error("complete-timeout must be positive")
    if args.busy_timeout <= 0:
        parser.error("busy-timeout must be positive")
    for value, name in (
        (args.start_delay, "start-delay"),
        (args.tail_gap, "tail-gap"),
        (args.final_settle, "final-settle"),
    ):
        if not math.isfinite(value) or value < 0:
            parser.error(f"{name} must be a finite, non-negative number")
    for value, name in (
        (args.complete_timeout, "complete-timeout"),
        (args.busy_timeout, "busy-timeout"),
    ):
        if not math.isfinite(value):
            parser.error(f"{name} must be a finite number")

    summary_path = args.out.with_suffix(".summary.json")
    if not args.force:
        if args.out.exists():
            parser.error(f"output exists: {args.out} (use --force)")
        if summary_path.exists():
            parser.error(f"summary exists: {summary_path} (use --force)")
    return args


def read_some(ser: serial.Serial, log_file: Any, buffer: bytearray) -> bytes:
    data = ser.read(4096)
    if data:
        log_file.write(data)
        log_file.flush()
        buffer.extend(data)
    return data


def transaction_result(
    index: int,
    attempt: int,
    request_id: bytes | None,
    window: bytearray,
    complete: bool,
) -> dict[str, Any]:
    return {
        "index": index,
        "attempts": attempt,
        "request_id": request_id.decode("ascii") if request_id else None,
        "complete": complete,
        "window_no_mem": bool(NO_MEM_RE.search(window)),
        "window_reset_marker": bool(RESET_RE.search(window)),
    }


def run_one(
    ser: serial.Serial,
    log_file: Any,
    index: int,
    args: argparse.Namespace,
) -> dict[str, Any]:
    attempt = 0
    busy_deadline = time.monotonic() + args.busy_timeout
    window = bytearray()

    while time.monotonic() < busy_deadline:
        attempt += 1
        window = bytearray()
        accepted: bytes | None = None
        ser.write(b"rf query\r\n")
        deadline = time.monotonic() + args.complete_timeout

        while time.monotonic() < deadline:
            read_some(ser, log_file, window)
            if RESET_RE.search(window):
                return transaction_result(
                    index, attempt, accepted, window, False
                )

            accepted_matches = list(ACCEPTED_RE.finditer(window))
            if accepted_matches:
                accepted = accepted_matches[-1].group(1)
            if accepted:
                for complete_match in COMPLETE_RE.finditer(window):
                    if complete_match.group(1) == accepted:
                        return transaction_result(
                            index, attempt, accepted, window, True
                        )
            if accepted is None and BUSY_RE.search(window):
                time.sleep(0.5)
                break
            time.sleep(0.01)

        if accepted is not None:
            return transaction_result(index, attempt, accepted, window, False)
        time.sleep(0.5)

    return transaction_result(index, attempt, None, window, False)


def build_summary(
    args: argparse.Namespace,
    results: list[dict[str, Any]],
    interrupted: str | None,
) -> dict[str, Any]:
    whole_log = args.out.read_bytes() if args.out.exists() else b""

    # Only events after the first console command belong to this test. This
    # keeps pre-test stack output from being counted as additional traffic.
    first_command = whole_log.find(b"rf query")
    test_log = whole_log[first_command:] if first_command >= 0 else b""

    send_matches = list(SEND_RE.finditer(test_log))
    send_statuses = [match.group(1).decode("ascii") for match in send_matches]
    accepted_ids = list(ACCEPTED_RE.findall(test_log))
    complete_ids = list(COMPLETE_RE.findall(test_log))
    report_count = len(REPORT_RE.findall(test_log))
    confirm_statuses = [
        int(match.group(1)) for match in CONFIRM_RE.finditer(test_log)
    ]
    post_start_reset_markers = len(RESET_RE.findall(test_log))

    for result, confirm_status in zip(results, confirm_statuses):
        result["confirm_status"] = confirm_status
        result["confirm_success"] = confirm_status == 0

    status_counts = {
        str(status): confirm_statuses.count(status)
        for status in sorted(set(confirm_statuses))
    }
    complete_count = sum(1 for result in results if result["complete"])
    no_mem_count = len(NO_MEM_RE.findall(whole_log))
    failure_reasons: list[str] = []

    if interrupted:
        failure_reasons.append(f"interrupted: {interrupted}")
    if complete_count != args.count:
        failure_reasons.append("not every console transaction completed")
    if len(send_statuses) != args.count:
        failure_reasons.append("raw APS send count does not match command count")
    if any(status != "ESP_OK" for status in send_statuses):
        failure_reasons.append("at least one raw APS send entry failed")
    if len(accepted_ids) != args.count or len(set(accepted_ids)) != args.count:
        failure_reasons.append("accepted request IDs are missing or duplicated")
    if len(complete_ids) != args.count or len(set(complete_ids)) != args.count:
        failure_reasons.append("complete request IDs are missing or duplicated")
    if set(accepted_ids) != set(complete_ids):
        failure_reasons.append("accepted and complete request IDs do not match")
    if report_count != args.count:
        failure_reasons.append("node RF power report count does not match")
    if len(confirm_statuses) != args.count:
        failure_reasons.append("raw APS confirm count does not match send count")
    if no_mem_count != 0:
        failure_reasons.append("ESP_ERR_NO_MEM was observed")
    if post_start_reset_markers != 0:
        failure_reasons.append("reset/panic marker observed after test start")
    if args.require_confirm_success and any(confirm_statuses):
        failure_reasons.append("at least one APS confirm status is non-zero")

    return {
        "status": "PASS" if not failure_reasons else "FAIL",
        "failure_reasons": failure_reasons,
        "count": args.count,
        "complete": complete_count,
        "send": len(send_statuses),
        "send_status_counts": {
            status: send_statuses.count(status)
            for status in sorted(set(send_statuses))
        },
        "accepted": len(accepted_ids),
        "accepted_unique": len(set(accepted_ids)),
        "complete_ids": len(complete_ids),
        "complete_unique": len(set(complete_ids)),
        "node_reports": report_count,
        "confirms": len(confirm_statuses),
        "confirm_success": sum(1 for status in confirm_statuses if status == 0),
        "confirm_status_counts": status_counts,
        "no_mem_marker_count": no_mem_count,
        "post_start_reset_marker_count": post_start_reset_markers,
        "results": results,
    }


def write_summary(summary_path: Path, summary: dict[str, Any]) -> None:
    summary_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )


def main() -> int:
    args = parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    summary_path = args.out.with_suffix(".summary.json")

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.1
    ser.write_timeout = 2.0
    ser.dtr = False
    ser.rts = False
    ser.open()

    results: list[dict[str, Any]] = []
    interrupted: str | None = None

    try:
        with args.out.open("wb") as log_file:
            startup = bytearray()
            startup_end = time.monotonic() + args.start_delay
            while time.monotonic() < startup_end:
                read_some(ser, log_file, startup)
            if RESET_RE.search(startup):
                print("检测到测试开始前的启动/复位记录；该记录不计入失败。")

            for index in range(1, args.count + 1):
                result = run_one(ser, log_file, index, args)
                results.append(result)
                print(
                    f"[{index}/{args.count}] complete={result['complete']} "
                    f"no_mem={result['window_no_mem']} "
                    f"reset={result['window_reset_marker']} "
                    f"id={result['request_id']}",
                    flush=True,
                )
                if not result["complete"]:
                    break
                time.sleep(args.tail_gap)

            tail = bytearray()
            settle_end = time.monotonic() + args.final_settle
            while time.monotonic() < settle_end:
                read_some(ser, log_file, tail)
                if RESET_RE.search(tail):
                    break
    except BaseException as exc:  # Include KeyboardInterrupt for partial evidence.
        interrupted = f"{type(exc).__name__}: {exc}"
    finally:
        ser.close()

    summary = build_summary(args, results, interrupted)
    write_summary(summary_path, summary)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())