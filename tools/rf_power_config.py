"""运行时修改 Zigbee 节点发射功率，无需重新烧录固件。

推荐使用后端虚拟环境运行：
  server/backend/.venv/Scripts/python.exe tools/rf_power_config.py --help

密码优先从 MQTT_PASSWORD 环境变量读取；未设置时安全提示输入，不会出现在命令行历史中。
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import sys
import uuid
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
    from paho.mqtt.enums import CallbackAPIVersion
except ImportError as exc:  # pragma: no cover - user environment guidance
    raise SystemExit(
        "缺少 paho-mqtt，请用 server/backend/.venv/Scripts/python.exe 运行"
    ) from exc

SUPPORTED_POWERS = (-10, 0, 8, 14, 18, 20)


def positive_timeout(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("必须是数字") from exc
    if parsed <= 0 or parsed > 120:
        raise argparse.ArgumentTypeError("超时时间必须在 1 到 120 秒之间")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="免烧录查询/设置 Zigbee 节点发射功率"
    )
    parser.add_argument("--host", default=os.getenv("MQTT_HOST", "127.0.0.1"),
                        help="MQTT 地址，默认 127.0.0.1")
    parser.add_argument("--port", type=int,
                        default=int(os.getenv("MQTT_PORT", "1883")),
                        help="MQTT 端口，默认 1883")
    parser.add_argument("--prefix", default=os.getenv("MQTT_TOPIC_PREFIX", "iot-home"),
                        help="MQTT 主题前缀，默认 iot-home")
    parser.add_argument("--gateway", default="gw-001",
                        help="网关 ID，默认 gw-001")
    parser.add_argument("--node", default="zb-82cb",
                        help="节点 ID，默认 zb-82cb")
    parser.add_argument("--username", default=os.getenv("MQTT_USER") or os.getenv("MQTT_USERNAME"),
                        help="MQTT 用户名，也可用 MQTT_USER 环境变量")
    parser.add_argument("--timeout", type=positive_timeout, default=20.0,
                        help="等待网关确认秒数，默认 20")
    subparsers = parser.add_subparsers(dest="action", required=True)

    subparsers.add_parser("query", help="查询当前功率和模式")
    subparsers.add_parser("auto", help="恢复自动功率控制")

    set_parser = subparsers.add_parser("set", help="设置固定功率并锁定手动模式")
    set_parser.add_argument("--power", type=int, required=True,
                            choices=SUPPORTED_POWERS,
                            help="目标功率 dBm")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    command_topic = f"{args.prefix}/{args.gateway}/nodes/{args.node}/cmd"
    policy_topic = f"{args.prefix}/{args.gateway}/nodes/{args.node}/rf_policy"

    if args.action == "query":
        command_name = "rf_query_power"
        payload: dict[str, object] = {}
    elif args.action == "auto":
        command_name = "rf_set_auto"
        payload = {}
    else:
        command_name = "rf_set_power"
        payload = {"power_dbm": args.power}

    request_id = uuid.uuid4().hex[:16]
    message = {
        "command": command_name,
        "payload": payload,
        "request_id": request_id,
        "ts": datetime.now().isoformat(timespec="seconds"),
    }

    client = mqtt.Client(
        CallbackAPIVersion.VERSION2,
        client_id=f"rf-config-{uuid.uuid4().hex[:12]}",
    )
    if args.username:
        password = os.getenv("MQTT_PASSWORD")
        if password is None:
            password = getpass.getpass("请输入 MQTT 密码（输入时不显示）: ")
        client.username_pw_set(args.username, password)

    received: dict[str, object] = {}

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            client.subscribe(policy_topic, qos=1)
        else:
            print(f"连接 MQTT 失败：{reason_code}", file=sys.stderr)

    def on_message(client, userdata, msg):
        try:
            parsed = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if isinstance(parsed, dict):
            received.clear()
            received.update(parsed)

    client.on_connect = on_connect
    client.on_message = on_message

    try:
        client.connect(args.host, args.port, keepalive=30)
    except OSError as exc:
        print(f"无法连接 MQTT：{exc}", file=sys.stderr)
        return 2

    import time

    def confirmed_matches(state: dict[str, object]) -> bool:
        if (state.get("request_id") != request_id or
            state.get("status") != "confirmed"):
            return False
        mode = state.get("mode")
        power = state.get("power_dbm")
        if args.action == "query":
            return mode in ("auto", "manual") and isinstance(power, int)
        if args.action == "auto":
            return mode == "auto" and isinstance(power, int)
        return mode == "manual" and power == args.power

    client.loop_start()
    try:
        client.publish(command_topic, json.dumps(message).encode("utf-8"),
                       qos=1).wait_for_publish(timeout=args.timeout)
        end = time.monotonic() + args.timeout
        while time.monotonic() < end and not confirmed_matches(received):
            time.sleep(0.05)
    finally:
        client.loop_stop()
        client.disconnect()

    if not confirmed_matches(received):
        print("命令已发出，但在等待时间内没有收到与本次事务匹配的网关确认", file=sys.stderr)
        return 3

    mode = received.get("mode", "unknown")
    power = received.get("power_dbm", "?")
    print(f"节点 {args.node} 当前模式：{mode}，当前功率：{power} dBm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
