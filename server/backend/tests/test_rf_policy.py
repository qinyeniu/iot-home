"""射频功率策略：状态跟踪、REST API 与落库测试。"""

import unittest
from datetime import datetime

from fastapi import FastAPI
from fastapi.testclient import TestClient
from sqlalchemy import select
from sqlalchemy.ext.asyncio import (
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)
from sqlalchemy.pool import StaticPool

from app.models.database import Base, Device, Metric
from app.models.session import get_session
from app.routers.devices import router as devices_router
from app.services.mqtt import MQTTService
from app.services.rf_policy import (
    KIND_AUTO,
    KIND_MANUAL,
    KIND_QUERY,
    rf_policy_states,
)

DEVICE_ID = "gw-001-zb-82cb"


class RFPolicyStateTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        rf_policy_states.reset()

    async def asyncTearDown(self):
        rf_policy_states.reset()

    def _confirmed(self, request_id, mode, power):
        return {
            "status": "confirmed",
            "request_id": request_id,
            "mode": mode,
            "power_dbm": power,
        }

    async def test_query_confirmed_completes(self):
        rid = "a" * 16
        fut = rf_policy_states.register_waiter(
            rid, DEVICE_ID, KIND_QUERY, None
        )
        pending = rf_policy_states.update_state(
            DEVICE_ID, self._confirmed(rid, "auto", -10)
        )
        self.assertIsNotNone(pending)
        self.assertTrue(fut.done())

    async def test_manual_requires_matching_power(self):
        rid = "b" * 16
        fut = rf_policy_states.register_waiter(
            rid, DEVICE_ID, KIND_MANUAL, 0
        )
        # 功率不匹配：不能完成。
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed(rid, "manual", 8)
        )
        self.assertFalse(fut.done())
        # 模式 auto：不能完成。
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed(rid, "auto", 0)
        )
        self.assertFalse(fut.done())
        # 精确匹配：完成。
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed(rid, "manual", 0)
        )
        self.assertTrue(fut.done())

    async def test_auto_requires_auto_mode(self):
        rid = "c" * 16
        fut = rf_policy_states.register_waiter(
            rid, DEVICE_ID, KIND_AUTO, None
        )
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed(rid, "manual", -10)
        )
        self.assertFalse(fut.done())
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed(rid, "auto", -10)
        )
        self.assertTrue(fut.done())

    async def test_intermediate_status_does_not_complete(self):
        rid = "d" * 16
        fut = rf_policy_states.register_waiter(
            rid, DEVICE_ID, KIND_QUERY, None
        )
        rf_policy_states.update_state(
            DEVICE_ID,
            {"status": "querying", "request_id": rid,
             "mode": None, "power_dbm": None},
        )
        rf_policy_states.update_state(
            DEVICE_ID,
            {"status": "report_received", "request_id": rid,
             "mode": "auto", "power_dbm": -10},
        )
        self.assertFalse(fut.done())

    async def test_foreign_request_id_does_not_complete(self):
        rid = "e" * 16
        fut = rf_policy_states.register_waiter(
            rid, DEVICE_ID, KIND_QUERY, None
        )
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed("f" * 16, "auto", -10)
        )
        self.assertFalse(fut.done())

    async def test_invalid_power_rejected(self):
        rid = "0123456789abcdef"
        fut = rf_policy_states.register_waiter(
            rid, DEVICE_ID, KIND_QUERY, None
        )
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed(rid, "auto", 5)
        )
        self.assertFalse(fut.done())

    async def test_state_is_cached(self):
        rf_policy_states.update_state(
            DEVICE_ID, self._confirmed("0" * 16, "auto", -10)
        )
        state = rf_policy_states.get_state(DEVICE_ID)
        self.assertEqual(state["power_dbm"], -10)
        self.assertEqual(state["mode"], "auto")
        self.assertEqual(state["device_id"], DEVICE_ID)


class RFPolicyAPITests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        rf_policy_states.reset()
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
                    name="zb-82cb",
                    type="sensor",
                    status="online",
                )
            )
            await session.commit()

        app = FastAPI()
        app.include_router(devices_router)

        async def override_session():
            async with AsyncSession(self.engine) as session:
                yield session

        app.dependency_overrides[get_session] = override_session
        self.client = TestClient(app)

        import app.routers.devices as devices_module

        self.devices_module = devices_module
        self._old_publish = devices_module.mqtt_service.publish_command

    async def asyncTearDown(self):
        self.devices_module.mqtt_service.publish_command = self._old_publish
        rf_policy_states.reset()
        await self.engine.dispose()

    def _install_publish_reply(self, mode="auto", power=-10):
        """模拟网关在命令发出后立即回报 confirmed。"""

        captured = {}

        async def fake_publish(device_id, command, payload, request_id=None):
            captured["request_id"] = request_id
            rf_policy_states.update_state(
                DEVICE_ID,
                {
                    "status": "confirmed",
                    "request_id": request_id,
                    "mode": mode,
                    "power_dbm": power,
                },
            )
            return True

        self.devices_module.mqtt_service.publish_command = fake_publish
        return captured

    def test_get_unknown_before_any_report(self):
        resp = self.client.get(f"/api/devices/{DEVICE_ID}/rf-policy")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "unknown")

    def test_get_device_404(self):
        resp = self.client.get("/api/devices/no-such/rf-policy")
        self.assertEqual(resp.status_code, 404)

    def test_manual_without_power_422(self):
        resp = self.client.post(
            f"/api/devices/{DEVICE_ID}/rf-policy",
            json={"mode": "manual"},
        )
        self.assertEqual(resp.status_code, 422)

    def test_manual_with_invalid_power_422(self):
        resp = self.client.post(
            f"/api/devices/{DEVICE_ID}/rf-policy",
            json={"mode": "manual", "power_dbm": 5},
        )
        self.assertEqual(resp.status_code, 422)

    def test_set_manual_success(self):
        captured = self._install_publish_reply(mode="manual", power=0)
        resp = self.client.post(
            f"/api/devices/{DEVICE_ID}/rf-policy",
            json={"mode": "manual", "power_dbm": 0},
        )
        self.assertEqual(resp.status_code, 200, resp.text)
        body = resp.json()
        self.assertEqual(body["mode"], "manual")
        self.assertEqual(body["power_dbm"], 0)
        self.assertEqual(body["request_id"], captured["request_id"])

    def test_set_auto_success(self):
        self._install_publish_reply(mode="auto", power=-10)
        resp = self.client.post(
            f"/api/devices/{DEVICE_ID}/rf-policy",
            json={"mode": "auto"},
        )
        self.assertEqual(resp.status_code, 200, resp.text)
        self.assertEqual(resp.json()["mode"], "auto")

    def test_timeout_returns_504(self):
        async def fake_publish(device_id, command, payload, request_id=None):
            return True

        self.devices_module.mqtt_service.publish_command = fake_publish
        resp = self.client.post(
            f"/api/devices/{DEVICE_ID}/rf-policy?timeout=1",
            json={"mode": "auto"},
        )
        self.assertEqual(resp.status_code, 504)

    def test_publish_failure_returns_500(self):
        async def fake_publish(device_id, command, payload, request_id=None):
            return False

        self.devices_module.mqtt_service.publish_command = fake_publish
        resp = self.client.post(
            f"/api/devices/{DEVICE_ID}/rf-policy",
            json={"mode": "auto"},
        )
        self.assertEqual(resp.status_code, 500)


class RFPolicyPersistenceTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        rf_policy_states.reset()
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
                    name="zb-82cb",
                    type="sensor",
                    status="online",
                )
            )
            await session.commit()

        import app.services.mqtt as mqtt_module

        self.mqtt_module = mqtt_module
        self._old_factory = mqtt_module.async_session_factory
        mqtt_module.async_session_factory = self.factory
        self.service = MQTTService()

    async def asyncTearDown(self):
        self.mqtt_module.async_session_factory = self._old_factory
        rf_policy_states.reset()
        await self.engine.dispose()

    async def _metrics(self):
        async with self.factory() as session:
            result = await session.execute(select(Metric))
            return result.scalars().all()

    async def test_confirmed_writes_metrics(self):
        await self.service._handle_rf_policy(
            "gw-001",
            "zb-82cb",
            {"status": "confirmed", "request_id": "x" * 16,
             "mode": "manual", "power_dbm": 8},
        )
        metrics = await self._metrics()
        values = {(m.metric, m.value) for m in metrics}
        self.assertIn(("tx_power_dbm", 8.0), values)
        self.assertIn(("rf_mode", 1.0), values)

    async def test_querying_does_not_write_metrics(self):
        await self.service._handle_rf_policy(
            "gw-001",
            "zb-82cb",
            {"status": "querying", "request_id": "y" * 16,
             "mode": None, "power_dbm": None},
        )
        self.assertEqual(await self._metrics(), [])

    async def test_auto_mode_encoded_zero(self):
        await self.service._handle_rf_policy(
            "gw-001",
            "zb-82cb",
            {"status": "confirmed", "request_id": "z" * 16,
             "mode": "auto", "power_dbm": -10},
        )
        metrics = await self._metrics()
        values = {(m.metric, m.value) for m in metrics}
        self.assertIn(("rf_mode", 0.0), values)
