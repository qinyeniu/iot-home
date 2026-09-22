"""设备能力推导测试。"""

import unittest
from datetime import datetime, timedelta

from sqlalchemy.ext.asyncio import (
    AsyncSession,
    create_async_engine,
)
from sqlalchemy.pool import StaticPool

from app.models.database import Base, Device, Metric
from app.services.capabilities import (
    CAPABILITY_WINDOW_DAYS,
    get_capabilities,
    get_capabilities_map,
)

NOW = datetime(2026, 9, 22, 12, 0, 0)


class CapabilitiesTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.engine = create_async_engine(
            "sqlite+aiosqlite:///:memory:",
            poolclass=StaticPool,
        )
        async with self.engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)
        self.session = AsyncSession(self.engine)
        for device_id in ("dev-sensor", "dev-combo", "dev-empty", "dev-stale"):
            self.session.add(
                Device(
                    id=device_id,
                    name=device_id,
                    type="sensor",
                    status="online",
                )
            )
        await self.session.commit()

    async def asyncTearDown(self):
        await self.session.aclose()

    async def _add_metric(self, device_id, metric, at):
        self.session.add(
            Metric(
                device_id=device_id,
                metric=metric,
                value=1.0,
                ts=at,
                received_at=at,
            )
        )

    async def test_sensor_only(self):
        for metric in ("temperature", "humidity", "lux"):
            await self._add_metric("dev-sensor", metric, NOW)
        await self.session.commit()

        cap = await get_capabilities(self.session, "dev-sensor", now=NOW)
        self.assertEqual(
            cap,
            {"sensors": ["humidity", "lux", "temperature"], "switch": False},
        )

    async def test_sensor_and_switch(self):
        for metric in ("temperature", "on_off"):
            await self._add_metric("dev-combo", metric, NOW)
        await self.session.commit()

        cap = await get_capabilities(self.session, "dev-combo", now=NOW)
        self.assertEqual(
            cap,
            {"sensors": ["temperature"], "switch": True},
        )

    async def test_no_metrics(self):
        cap = await get_capabilities(self.session, "dev-empty", now=NOW)
        self.assertEqual(cap, {"sensors": [], "switch": False})

    async def test_stale_metrics_excluded(self):
        stale_at = NOW - timedelta(days=CAPABILITY_WINDOW_DAYS + 1)
        await self._add_metric("dev-stale", "temperature", stale_at)
        await self._add_metric("dev-stale", "on_off", stale_at)
        await self.session.commit()

        cap = await get_capabilities(self.session, "dev-stale", now=NOW)
        self.assertEqual(cap, {"sensors": [], "switch": False})

    async def test_batch_map(self):
        await self._add_metric("dev-sensor", "temperature", NOW)
        await self._add_metric("dev-combo", "on_off", NOW)
        await self.session.commit()

        cap_map = await get_capabilities_map(
            self.session,
            ["dev-sensor", "dev-combo", "dev-empty"],
            now=NOW,
        )
        self.assertTrue(cap_map["dev-sensor"]["sensors"])
        self.assertFalse(cap_map["dev-sensor"]["switch"])
        self.assertTrue(cap_map["dev-combo"]["switch"])
        self.assertEqual(cap_map["dev-empty"],
                         {"sensors": [], "switch": False})

    async def test_empty_input(self):
        self.assertEqual(await get_capabilities_map(self.session, []), {})
