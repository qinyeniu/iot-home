"""API 查询参数边界与 MQTT on_off 布尔值拒绝测试。"""

import unittest
from datetime import datetime

from fastapi import FastAPI
from fastapi.testclient import TestClient
from sqlalchemy.ext.asyncio import (
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)
from sqlalchemy.pool import StaticPool

from app.models.database import Base, Command, Device
from app.models.session import get_session
from app.routers.devices import router as devices_router
from app.services.mqtt import MQTTService

DEVICE_ID = "gw-001-zb-test"


class MetricsQueryValidationTests(unittest.TestCase):
    def setUp(self):
        import os
        import tempfile

        from sqlalchemy import create_engine

        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        # 先用同步驱动建表并插入设备；异步会话随后读同一个文件。
        sync_engine = create_engine(
            f"sqlite:///{self.db_path}",
        )
        Base.metadata.create_all(sync_engine)
        with sync_engine.begin() as conn:
            conn.execute(
                Device.__table__.insert().values(
                    id=DEVICE_ID,
                    name="测试设备",
                    type="sensor",
                    status="online",
                )
            )
        sync_engine.dispose()

        self.engine = create_async_engine(
            f"sqlite+aiosqlite:///{self.db_path}",
        )

        app = FastAPI()
        app.include_router(devices_router)

        async def override_session():
            async with AsyncSession(self.engine) as session:
                yield session

        app.dependency_overrides[get_session] = override_session
        self.client = TestClient(app)

    def tearDown(self):
        import asyncio
        import os

        asyncio.run(self.engine.dispose())
        os.remove(self.db_path)

    def test_unknown_device_404(self):
        resp = self.client.get("/api/devices/no-such-device/metrics")
        self.assertEqual(resp.status_code, 404)

    def test_hours_must_be_positive(self):
        resp = self.client.get(
            f"/api/devices/{DEVICE_ID}/metrics?hours=0"
        )
        self.assertEqual(resp.status_code, 422)
        resp = self.client.get(
            f"/api/devices/{DEVICE_ID}/metrics?hours=-1"
        )
        self.assertEqual(resp.status_code, 422)

    def test_limit_must_be_positive(self):
        resp = self.client.get(
            f"/api/devices/{DEVICE_ID}/metrics?limit=0"
        )
        self.assertEqual(resp.status_code, 422)

    def test_valid_query_ok(self):
        resp = self.client.get(
            f"/api/devices/{DEVICE_ID}/metrics?hours=24&limit=10"
        )
        self.assertEqual(resp.status_code, 200)


class OnOffBooleanRejectionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.engine = create_async_engine(
            "sqlite+aiosqlite:///:memory:",
            poolclass=StaticPool,
        )
        async with self.engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)
        self.factory = async_sessionmaker(self.engine, expire_on_commit=False)
        async with self.factory() as session:
            session.add(
                Device(
                    id=DEVICE_ID,
                    name="测试设备",
                    type="sensor",
                    status="online",
                )
            )
            session.add(
                Command(
                    device_id=DEVICE_ID,
                    command="on",
                    status="sent",
                    sent_at=datetime.now(),
                )
            )
            await session.commit()

        import app.services.mqtt as mqtt_module

        self.old_factory = mqtt_module.async_session_factory
        mqtt_module.async_session_factory = self.factory
        self.mqtt_module = mqtt_module
        self.service = MQTTService()

    async def asyncTearDown(self):
        self.mqtt_module.async_session_factory = self.old_factory
        await self.engine.dispose()

    async def _command_status(self):
        from sqlalchemy import select

        async with self.factory() as session:
            return (
                await session.execute(select(Command.status))
            ).scalar_one()

    async def test_json_true_does_not_acknowledge(self):
        await self.service._handle_telemetry(
            "gw-001",
            "zb-test",
            {"data": {"on_off": True}},
        )
        self.assertEqual(await self._command_status(), "sent")

    async def test_integer_one_acknowledges(self):
        await self.service._handle_telemetry(
            "gw-001",
            "zb-test",
            {"data": {"on_off": 1}},
        )
        self.assertEqual(await self._command_status(), "acknowledged")
