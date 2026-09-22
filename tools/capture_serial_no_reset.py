"""Capture serial output without actively pulsing RTS/DTR.

Note: opening an ESP32 USB-Serial/JTAG port may still reset the board depending
on the driver/ROM wiring; this helper only avoids an explicit reset pulse.
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from pathlib import Path

import serial


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture a serial log without explicitly pulsing RTS/DTR."
    )
    parser.add_argument("port", help="serial port, for example COM5")
    parser.add_argument("out", type=Path, help="output binary log path")
    parser.add_argument(
        "duration",
        type=float,
        nargs="?",
        default=45.0,
        help="capture duration in seconds (default: 45)",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="overwrite the output file if it already exists",
    )
    args = parser.parse_args()

    if not math.isfinite(args.duration) or args.duration < 0:
        parser.error("duration must be a finite non-negative number")
    if args.out.exists() and not args.overwrite:
        parser.error(f"output file already exists: {args.out} (use --overwrite)")
    return args


def capture(port: str, out: Path, duration: float) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    stdout_ok = True

    with serial.Serial(
        port=port,
        baudrate=115200,
        timeout=1,
    ) as ser, out.open("wb") as log_file:
        # pyserial does not accept initial line-state constructor arguments.
        # Set them immediately after open and never pulse them afterwards.
        ser.dtr = False
        ser.rts = False
        start = time.monotonic()
        while time.monotonic() - start < duration:
            data = ser.read(4096)
            if not data:
                continue

            log_file.write(data)
            log_file.flush()
            if stdout_ok:
                try:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                except BrokenPipeError:
                    stdout_ok = False

    if stdout_ok:
        print(f"\n[CAPTURE DONE] -> {out}", file=sys.stderr)


def main() -> int:
    args = parse_args()
    try:
        capture(args.port, args.out, args.duration)
    except (serial.SerialException, OSError) as exc:
        print(f"serial capture failed: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nserial capture interrupted; log file was flushed", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
