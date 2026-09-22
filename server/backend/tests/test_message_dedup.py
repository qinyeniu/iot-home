"""message_id 幂等去重与保留期清理测试。"""

import asyncio
import unittest
from datetime import datetime, timedelta

from sqlalchemy import delete, func, select
from sqlalchemy.exc import OperationalError
from sqlalchemy.ext.asyncio import async_sessionmaker, create_async_engine
from sqlalchemy.pool import StaticPool

import app.services.mqtt as mqtt_module
from app.models.database import Base, Command, Device, Metric, ProcessedMessage
from app.services.mqtt import BadMessageError, MQTTService


class MessageDedupTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.engine = create_async_engine(
            "sqlite+aiosqlite:///:memory:",
            poolclass=StaticPool,
        )
        async with self.engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)

        self.factory = async_sessionmaker(
            self.engine, expire_on_commit=False
        )
        self.old_factory = mqtt_module.async_session_factory
        mqtt_module.async_session_factory = self.factory
        self.service = MQTTService()
        self.service._message_queue = asyncio.Queue()

    async def asyncTearDown(self):
        mqtt_module.async_session_factory = self.old_factory
        await self.engine.dispose()

    async def _send(self, node, payload, retained=False):
        await self.service._handle_telemetry(
            "gw-001", node, payload, retained=retained
        )

    async def _metric_count(self):
        async with self.factory() as session:
            return (
                await session.execute(select(func.count(Metric.id)))
            ).scalar_one()

    async def _processed_count(self):
        async with self.factory() as session:
            return (
                await session.execute(
                    select(func.count(ProcessedMessage.message_id))
                )
            ).scalar_one()

    async def test_duplicate_message_id_inserts_once(self):
        payload = {
            "message_id": "msg-001",
            "ts": "2026-09-22T10:00:00",
            "data": {"temperature": 25.0},
        }

        await self._send("zb-001", payload)
        await self._send("zb-001", payload)

        self.assertEqual(await self._metric_count(), 1)
        self.assertEqual(await self._processed_count(), 1)

    async def test_duplicate_on_off_acknowledges_only_first_time(self):
        async with self.factory() as session:
            session.add(
                Device(
                    id="gw-001-zb-switch",
                    name="开关节点",
                    type="sensor",
                    status="online",
                )
            )
            session.add(
                Command(
                    device_id="gw-001-zb-switch",
                    command="on",
                    status="sent",
                    sent_at=datetime.now(),
                )
            )
            await session.commit()

        payload = {
            "message_id": "switch-ack-001",
            "data": {"on_off": 1},
        }
        await self._send("zb-switch", payload)
        await self._send("zb-switch", payload)

        async with self.factory() as session:
            statuses = (
                await session.execute(select(Command.status))
            ).scalars().all()
        self.assertEqual(statuses, ["acknowledged"])
        self.assertEqual(await self._metric_count(), 1)

    async def test_same_message_id_from_other_device_rejected(self):
        payload = {
            "message_id": "shared-id",
            "data": {"temperature": 20.0},
        }
        await self._send("zb-a", payload)

        with self.assertRaises(BadMessageError):
            await self._send("zb-b", payload)

        self.assertEqual(await self._metric_count(), 1)
        async with self.factory() as session:
            device_b = await session.get(Device, "gw-001-zb-b")
        self.assertIsNone(device_b)

    async def test_invalid_message_id_values_rejected(self):
        for invalid_id in ("bad id", "", 123):
            with self.subTest(invalid_id=invalid_id):
                with self.assertRaises(BadMessageError):
                    await self._send(
                        "zb-invalid",
                        {
                            "message_id": invalid_id,
                            "data": {"temperature": 21.0},
                        },
                    )
        self.assertEqual(await self._metric_count(), 0)

    async def test_missing_message_id_stays_backward_compatible(self):
        payload = {"data": {"temperature": 22.0}}

        await self._send("zb-legacy", payload)
        await self._send("zb-legacy", payload)

        self.assertEqual(await self._metric_count(), 2)
        self.assertEqual(await self._processed_count(), 0)

    async def test_ack_sent_only_after_successful_processing(self):
        class FakeUnderlying:
            def __init__(self):
                self.acks = []

            def ack(self, mid, qos):
                self.acks.append((mid, qos))
                return 0

        class FakeAckMessage:
            def __init__(self):
                self.topic = "iot-home/gw-001/nodes/zb-ack/telemetry"
                self.payload = b"{}"
                self.qos = 1
                self.retain = False
                self.mid = 9
                self._mqtt_underlying = underlying

        underlying = FakeUnderlying()

        async def handler(message):
            # 处理尚未完成时不能提前确认。
            self.assertEqual(underlying.acks, [])

        self.service._handle_message = handler
        await self.service._process_with_retry(FakeAckMessage())
        self.assertEqual(underlying.acks, [(9, 1)])

    async def test_bad_message_acknowledged_once(self):
        class FakeUnderlying:
            def __init__(self):
                self.acks = []

            def ack(self, mid, qos):
                self.acks.append((mid, qos))
                return 0

        class FakeAckMessage:
            def __init__(self):
                self.topic = "t"
                self.payload = b"{}"
                self.qos = 1
                self.retain = False
                self.mid = 10
                self._mqtt_underlying = underlying

        underlying = FakeUnderlying()

        async def handler(message):
            raise BadMessageError("bad")

        self.service._handle_message = handler
        await self.service._process_with_retry(FakeAckMessage())
        self.assertEqual(underlying.acks, [(10, 1)])

    async def test_ack_waits_until_db_retry_succeeds(self):
        class FakeUnderlying:
            def __init__(self):
                self.acks = []

            def ack(self, mid, qos):
                self.acks.append((mid, qos))
                return 0

        class FakeAckMessage:
            def __init__(self):
                self.topic = "t"
                self.payload = b"{}"
                self.qos = 1
                self.retain = False
                self.mid = 11
                self._mqtt_underlying = underlying

        underlying = FakeUnderlying()
        calls = {"n": 0}

        async def handler(message):
            calls["n"] += 1
            self.assertEqual(underlying.acks, [])
            if calls["n"] < 2:
                raise OperationalError(None, None, Exception("boom"))

        self.service._handle_message = handler
        await self.service._process_with_retry(FakeAckMessage())
        self.assertEqual(calls["n"], 2)
        self.assertEqual(underlying.acks, [(11, 1)])

    async def test_dead_letter_saved_then_acknowledged(self):
        class FakeUnderlying:
            def __init__(self):
                self.acks = []

            def ack(self, mid, qos):
                self.acks.append((mid, qos))
                return 0

        class FakeAckMessage:
            def __init__(self):
                self.topic = "t"
                self.payload = b"{}"
                self.qos = 1
                self.retain = False
                self.mid = 12
                self._mqtt_underlying = underlying

        underlying = FakeUnderlying()

        async def handler(message):
            raise OperationalError(None, None, Exception("boom"))

        def write_dead_letter(message, exc):
            return True

        self.service._handle_message = handler
        self.service._write_dead_letter = write_dead_letter
        await self.service._process_with_retry(FakeAckMessage())
        self.assertEqual(underlying.acks, [(12, 1)])

    async def test_cleanup_deletes_only_expired_messages(self):
        now = datetime.now()
        async with self.factory() as session:
            session.add_all([
                ProcessedMessage(
                    message_id="old",
                    device_id="gw-001-zb-old",
                    processed_at=now - timedelta(days=31),
                ),
                ProcessedMessage(
                    message_id="new",
                    device_id="gw-001-zb-new",
                    processed_at=now,
                ),
            ])
            await session.commit()

        deleted = await self.service._cleanup_old_processed_messages_once()

        self.assertEqual(deleted, 1)
        async with self.factory() as session:
            remaining = (
                await session.execute(
                    select(ProcessedMessage.message_id)
                )
            ).scalars().all()
        self.assertEqual(remaining, ["new"])


if __name__ == "__main__":
    unittest.main()
