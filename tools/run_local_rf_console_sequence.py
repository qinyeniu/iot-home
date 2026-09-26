"""Drive the gateway experimental local RF console and correlate each reply."""
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path
from typing import Any

import serial

COMMANDS: tuple[dict[str, Any], ...] = (
    {"name": "query", "command": "rf query", "mode": None, "power": None},
    {"name": "set_0", "command": "rf set 0", "mode": "manual", "power": 0},
    {"name": "set_-10", "command": "rf set -10", "mode": "manual",
     "power": -10},
    {"name": "auto", "command": "rf auto", "mode": "auto", "power": None},
)
SUPPORTED_POWERS = {-10, 0, 8, 14, 18, 20}

ACCEPTED_RE = re.compile(
    rb"LOCAL_RF_ACCEPTED (?:query|set -?\d+|auto) request_id=([0-9a-f]{16})"
)
REPORT_RE = re.compile(
    rb"RF power: report accepted from \S+ to (manual|auto)/(-?\d+) dBm "
    rb"seq=\d+"
)
COMPLETE_RE = re.compile(
    rb"LOCAL_RF_COMPLETE request_id=([0-9a-f]{16})"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run QUERY/SET/AUTO through the gateway local RF console."
    )
    parser.add_argument("port", help="gateway serial port, e.g. COM6")
    parser.add_argument("out", type=Path, help="serial log path")
    parser.add_argument("--duration", type=float, default=30.0,
                        help="capture time after final command (seconds)")
    parser.add_argument("--start-delay", type=float, default=5.0,
                        help="wait after opening before first command")
    parser.add_argument("--confirm-timeout", type=float, default=25.0,
                        help="wait for completion per command")
    parser.add_argument("--tail-gap", type=float, default=2.0,
                        help="pause after confirmation before next command")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    if args.out.exists() and not args.overwrite:
        parser.error(f"output exists: {args.out} (use --overwrite)")
    for name in ("duration", "start_delay", "confirm_timeout", "tail_gap"):
        if getattr(args, name) < 0:
            parser.error(f"{name} must be non-negative")
    return args


def emit(data: bytes, log_file: Any) -> None:
    if not data:
        return
    log_file.write(data)
    log_file.flush()
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def pump_for(ser: serial.Serial, log_file: Any, seconds: float) -> None:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        emit(ser.read(4096), log_file)


def validate_report(buffer: bytearray, expected: dict[str, Any]) -> None:
    reports = list(REPORT_RE.finditer(buffer))
    if not reports:
        raise RuntimeError("accepted RF report was not found")

    mode = reports[-1].group(1).decode("ascii")
    power = int(reports[-1].group(2))
    if expected["mode"] is not None and mode != expected["mode"]:
        raise RuntimeError(
            f"unexpected mode {mode}; wanted {expected['mode']}"
        )
    if expected["power"] is not None:
        if power != expected["power"]:
            raise RuntimeError(
                f"unexpected power {power}; wanted {expected['power']}"
            )
    elif power not in SUPPORTED_POWERS:
        raise RuntimeError(f"unexpected reported power {power}")


def wait_for_transaction(
    ser: serial.Serial,
    log_file: Any,
    expected: dict[str, Any],
    timeout: float,
) -> None:
    buffer = bytearray()
    deadline = time.monotonic() + timeout
    accepted = None

    while time.monotonic() < deadline:
        emit(ser.read(4096), log_file)
        if accepted is None:
            accepted_matches = list(ACCEPTED_RE.finditer(buffer))
            if accepted_matches:
                accepted = accepted_matches[-1].group(1)
        if accepted is not None:
            for complete in COMPLETE_RE.finditer(buffer):
                if complete.group(1) == accepted:
                    validate_report(buffer, expected)
                    return

    if accepted is None:
        raise TimeoutError(f"{expected['name']}: accepted line not received")
    raise TimeoutError(
        f"{expected['name']}: completion for request_id="
        f"{accepted.decode('ascii')} not received"
    )


def main() -> int:
    args = parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)

    # Configure DTR/RTS before open() so merely starting this script does not
    # pulse the board's reset line.
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.write_timeout = 2.0
    ser.dtr = False
    ser.rts = False
    ser.open()

    try:
        with args.out.open("wb") as log_file:
            pump_for(ser, log_file, args.start_delay)

            for index, expected in enumerate(COMMANDS):
                print(f"\n[LOCAL RF SEND] {expected['name']}: "
                      f"{expected['command']}", file=sys.stderr)
                ser.write(f"{expected['command']}\r\n".encode("ascii"))
                try:
                    wait_for_transaction(
                        ser, log_file, expected, args.confirm_timeout
                    )
                except (RuntimeError, TimeoutError) as exc:
                    print(f"\n[LOCAL RF FAILED] {exc}", file=sys.stderr)
                    return 1
                print(f"\n[LOCAL RF CONFIRMED] {expected['name']}",
                      file=sys.stderr)

                if index < len(COMMANDS) - 1:
                    pump_for(ser, log_file, args.tail_gap)

            pump_for(ser, log_file, args.duration)
    finally:
        ser.close()

    print(f"\n[LOCAL RF DONE] {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
