"""MQTT 可靠性逻辑测试：重试、死信、坏消息、retained 幻影、关停 drain。"""

import asyncio
import json
import os
import tempfile
import unittest

from sqlalchemy.exc import OperationalError
from sqlalchemy.ext.asyncio import (
    AsyncSession,
    create_async_engine,
)
from sqlalchemy.pool import StaticPool

import app.services.mqtt as mqtt_module
from app.models.database import Base, Device


def db_error():
    return OperationalError(None, None, Exception("boom"))


class FakeMessage:
    def __init__(self, topic, payload=b"{}", qos=1, retain=False):
        self.topic = topic
        self.payload = payload
        self.qos = qos
        self.retain = retain


class RetryLogicTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.service = mqtt_module.MQTTService()
        self.service._message_queue = asyncio.Queue()
        self._old_delay = mqtt_module.PROCESS_RETRY_BASE_DELAY
        mqtt_module.PROCESS_RETRY_BASE_DELAY = 0.01
        self._old_dl = mqtt_module.DEAD_LETTER_PATH
        self.fd, self.dl_path = tempfile.mkstemp(suffix=".jsonl")
        os.close(self.fd)
        mqtt_module.DEAD_LETTER_PATH = self.dl_path

    async def asyncTearDown(self):
        mqtt_module.PROCESS_RETRY_BASE_DELAY = self._old_delay
        mqtt_module.DEAD_LETTER_PATH = self._old_dl
        os.remove(self.dl_path)

    async def test_db_error_then_success_retries(self):
        calls = {"n": 0}

        async def handler(message):
            calls["n"] += 1
            if calls["n"] < 3:
                raise db_error()

        self.service._handle_message = handler
        await self.service._process_with_retry(FakeMessage("t"))
        self.assertEqual(calls["n"], 3)
        self.assertEqual(os.path.getsize(self.dl_path), 0)

    async def test_db_error_exhausted_writes_dead_letter(self):
        calls = {"n": 0}

        async def handler(message):
            calls["n"] += 1
            raise db_error()

        self.service._handle_message = handler
        await self.service._process_with_retry(
            FakeMessage("iot-home/gw/nodes/zb-x/telemetry",
                        payload=b'{"data":{}}')
        )
        self.assertEqual(calls["n"], mqtt_module.MAX_PROCESS_ATTEMPTS)
        with open(self.dl_path, encoding="utf-8") as f:
            lines = [json.loads(line) for line in f if line.strip()]
        self.assertEqual(len(lines), 1)
        self.assertIn("boom", lines[0]["error"])

    async def test_bad_message_not_retried(self):
        calls = {"n": 0}

        async def handler(message):
            calls["n"] += 1
            raise mqtt_module.BadMessageError("bad")

        self.service._handle_message = handler
        await self.service._process_with_retry(FakeMessage("t"))
        self.assertEqual(calls["n"], 1)
        self.assertEqual(os.path.getsize(self.dl_path), 0)

    async def test_shutdown_drains_pending_message(self):
        processed = asyncio.Event()

        async def slow_handler(message):
            await asyncio.sleep(0.3)
            processed.set()

        queue = asyncio.Queue()
        service = mqtt_module.MQTTService()
        service._message_queue = queue
        service._handle_message = slow_handler
        service._worker_task = asyncio.create_task(
            service._process_messages()
        )
        await queue.put(FakeMessage("t"))
        await service._shutdown_worker()
        self.assertTrue(processed.is_set())
        self.assertEqual(queue.qsize(), 0)


class RetainedHandlingTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.engine = create_async_engine(
            "sqlite+aiosqlite:///:memory:",
            poolclass=StaticPool,
        )
        async with self.engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)
        self._old_factory = mqtt_module.async_session_factory
        mqtt_module.async_session_factory = self._factory()
        self.service = mqtt_module.MQTTService()

    def _factory(self):
        from sqlalchemy.ext.asyncio import async_sessionmaker

        return async_sessionmaker(self.engine, expire_on_commit=False)

    async def asyncTearDown(self):
        mqtt_module.async_session_factory = self._old_factory
        await self.engine.dispose()

    async def test_retained_status_does_not_create_device(self):
        await self.service._handle_status(
            "gw-x", "zb-ghost", {"status": "online"}, retained=True
        )
        async with self._factory()() as session:
            device = await session.get(Device, "gw-x-zb-ghost")
        self.assertIsNone(device)

    async def test_live_status_creates_device(self):
        await self.service._handle_status(
            "gw-x", "zb-live", {"status": "online"}, retained=False
        )
        async with self._factory()() as session:
            device = await session.get(Device, "gw-x-zb-live")
        self.assertIsNotNone(device)
        self.assertEqual(device.status, "online")
        self.assertIsNotNone(device.last_seen)

    async def test_retained_telemetry_keeps_last_seen_none(self):
        await self.service._handle_telemetry(
            "gw-x", "zb-r", {"data": {"temperature": 25.0}},
            retained=True,
        )
        async with self._factory()() as session:
            device = await session.get(Device, "gw-x-zb-r")
        self.assertIsNotNone(device)
        self.assertIsNone(device.last_seen)

    async def test_invalid_status_normalized_to_unknown(self):
        await self.service._handle_status(
            "gw-x", "zb-u", {"status": "weird"}, retained=False
        )
        async with self._factory()() as session:
            device = await session.get(Device, "gw-x-zb-u")
        self.assertEqual(device.status, "unknown")
        self.assertIsNotNone(device.last_seen)
