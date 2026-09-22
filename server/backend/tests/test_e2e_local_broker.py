r"""本地 broker 端到端验证（默认跳过）。

需要本机允许监听 127.0.0.1:1884 并用虚拟环境里的 amqtt broker。
运行：

  $env:IOT_RUN_LOCAL_BROKER = "1"
  .\.venv\Scripts\python.exe -m pytest tests/test_e2e_local_broker.py -s

验证点：
1. 处理慢（模拟数据库延迟）+ 高频上报时，QoS1 消息不丢，只是积压；
2. broker 中途断连重启后，服务自动重连、发布端重投，消息最终不丢。
"""

import asyncio
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import async_sessionmaker, create_async_engine

import app.services.mqtt as mqtt_module
from app.config import settings
from app.models.database import Base, Device, Metric

BROKER_PORT = 1884
TOPIC = "iot-home/gw-001/nodes/zb-e2e/telemetry"

BROKER_CONFIG = """
listeners:
  default:
    type: tcp
    bind: 127.0.0.1:{port}
auth:
  allow_anonymous: true
""".format(port=BROKER_PORT)


def wait_for_port(host: str, port: int, timeout: float = 15.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return
        except OSError:
            time.sleep(0.25)
    raise RuntimeError(f"broker port {port} not ready")


def amqtt_exe() -> Path:
    scripts = Path(sys.executable).parent
    name = "amqtt.exe" if os.name == "nt" else "amqtt"
    path = scripts / name
    if not path.exists():
        raise unittest.SkipTest("虚拟环境中未找到 amqtt 可执行文件")
    return path


class Publisher:
    """模拟网关：QoS1 持续发遥测；broker 断连时自动重连并重投。"""

    def __init__(self, count: int, interval: float):
        import paho.mqtt.client as mqtt

        self._count = count
        self._interval = interval
        self._acknowledged = threading.Event()
        self.published_count = 0
        self.failed: list[int] = []

        self.client = mqtt.Client(
            client_id=f"e2e-publisher-{os.getpid()}",
            protocol=mqtt.MQTTv311,
        )
        self.client.reconnect_delay_set(min_delay=1, max_delay=5)
        self.client.on_connect = self._on_connect
        self.client.on_publish = self._on_publish

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            threading.Thread(target=self._publish_loop, daemon=True).start()

    def _on_publish(self, client, userdata, mid):
        self.published_count += 1
        if self.published_count >= self._count:
            self._acknowledged.set()

    def _publish_loop(self):
        for seq in range(self._count):
            payload = {
                "data": {
                    "temperature": 25.0 + seq * 0.01,
                    "humidity": 60.0,
                    "lux": seq,
                }
            }
            info = self.client.publish(TOPIC, json.dumps(payload), qos=1)
            if info.rc != 0:
                self.failed.append(seq)
            time.sleep(self._interval)

    def start(self):
        self.client.connect("127.0.0.1", BROKER_PORT, keepalive=30)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()

    def wait_all_published(self, timeout: float) -> bool:
        return self._acknowledged.wait(timeout)


@unittest.skipUnless(
    os.getenv("IOT_RUN_LOCAL_BROKER"),
    "设置 IOT_RUN_LOCAL_BROKER=1 才运行本地 broker 端到端测试",
)
class LocalBrokerE2ETests(unittest.IsolatedAsyncioTestCase):
    # paho-mqtt 的 socket 回调需要 add_reader/add_writer，仅
    # SelectorEventLoop 支持；Windows 默认的 Proactor 循环下 MQTT 无法收发。
    if sys.platform == "win32":
        def run(self, result=None):
            old_policy = asyncio.get_event_loop_policy()
            asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
            try:
                return super().run(result)
            finally:
                asyncio.set_event_loop_policy(old_policy)

    async def asyncSetUp(self) -> None:
        self.config_fd, self.config_path = tempfile.mkstemp(suffix=".yaml")
        with os.fdopen(self.config_fd, "w", encoding="utf-8") as f:
            f.write(BROKER_CONFIG)

        self.db_fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(self.db_fd)

        self.engine = create_async_engine(f"sqlite+aiosqlite:///{self.db_path}")
        async with self.engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)
        self.factory = async_sessionmaker(self.engine, expire_on_commit=False)

        self._old_factory = mqtt_module.async_session_factory
        mqtt_module.async_session_factory = self.factory

        self._old_host = settings.MQTT_HOST
        self._old_port = settings.MQTT_PORT
        self._old_user = settings.MQTT_USER
        self._old_password = settings.MQTT_PASSWORD
        settings.MQTT_HOST = "127.0.0.1"
        settings.MQTT_PORT = BROKER_PORT
        settings.MQTT_USER = None
        settings.MQTT_PASSWORD = None

        self.broker = subprocess.Popen(
            [str(amqtt_exe()), "-c", self.config_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port("127.0.0.1", BROKER_PORT)

        # 使用独立服务实例，注入每条消息的处理延迟，模拟慢数据库。
        self.service = mqtt_module.MQTTService()
        original_handler = self.service._handle_message

        async def slow_handler(message):
            await asyncio.sleep(self.process_delay)
            await original_handler(message)

        self.service._handle_message = slow_handler
        self.process_delay = 0.0
        self.service_task = asyncio.create_task(self.service.start())
        # 等服务完成连接与 QoS1 订阅。
        await asyncio.sleep(3)

    async def asyncTearDown(self) -> None:
        self.service_task.cancel()
        try:
            await self.service_task
        except asyncio.CancelledError:
            pass
        await self.engine.dispose()
        mqtt_module.async_session_factory = self._old_factory
        settings.MQTT_HOST = self._old_host
        settings.MQTT_PORT = self._old_port
        settings.MQTT_USER = self._old_user
        settings.MQTT_PASSWORD = self._old_password
        if self.broker.poll() is None:
            self.broker.terminate()
            try:
                self.broker.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.broker.kill()
        for path in (self.config_path, self.db_path):
            try:
                os.remove(path)
            except OSError:
                pass

    async def _metric_counts(self) -> dict[str, int]:
        async with self.factory() as session:
            result = await session.execute(
                select(Metric.metric, func.count(Metric.id)).group_by(Metric.metric)
            )
            return {metric: count for metric, count in result.all()}

    async def _wait_for_metrics(
        self, expected: int, timeout: float
    ) -> dict[str, int]:
        deadline = time.time() + timeout
        counts: dict[str, int] = {}
        while time.time() < deadline:
            counts = await self._metric_counts()
            if counts.get("temperature", 0) >= expected:
                return counts
            await asyncio.sleep(0.5)
        return counts

    async def test_slow_processing_no_loss(self) -> None:
        # 处理耗时 > 上报间隔：积压必然出现，但消息必须全部入库。
        self.process_delay = 0.15
        total = 60
        publisher = Publisher(total, interval=0.03)
        publisher.start()
        try:
            self.assertTrue(publisher.wait_all_published(timeout=30))
            counts = await self._wait_for_metrics(total, timeout=120)
        finally:
            publisher.stop()

        self.assertEqual(counts.get("temperature"), total)
        self.assertEqual(counts.get("humidity"), total)
        self.assertEqual(counts.get("lux"), total)
        self.assertEqual(publisher.failed, [])

    async def test_broker_restart_no_loss(self) -> None:
        total = 30
        publisher = None

        async def restart_broker() -> None:
            await asyncio.sleep(2)
            self.broker.terminate()
            self.broker.wait(timeout=10)
            await asyncio.sleep(8)  # 断网窗口：发布端积压、服务等待重连
            self.broker = subprocess.Popen(
                [str(amqtt_exe()), "-c", self.config_path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            wait_for_port("127.0.0.1", BROKER_PORT)

        restart_task = asyncio.create_task(restart_broker())
        publisher = Publisher(total, interval=0.2)
        publisher.start()
        try:
            self.assertTrue(publisher.wait_all_published(timeout=60))
            counts = await self._wait_for_metrics(total, timeout=120)
        finally:
            publisher.stop()
            await restart_task

        self.assertEqual(counts.get("temperature"), total)
        self.assertEqual(counts.get("humidity"), total)
        self.assertEqual(counts.get("lux"), total)
