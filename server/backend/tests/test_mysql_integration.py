r"""MySQL/InnoDB 并发开关命令集成测试。

默认跳过。需要真实 MySQL 时设置环境变量，例如：

PowerShell:
  $env:IOT_TEST_MYSQL_URL = "mysql+aiomysql://user:password@127.0.0.1:3306/iot_home_test?charset=utf8mb4"
  .\.venv\Scripts\python -m unittest tests.test_mysql_integration -v
"""

import asyncio
import os
import unittest
from datetime import datetime

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

from app.models.database import Base
from app.models.database import Command
from app.routers.devices import send_command
from app.routers.devices import CommandRequest
from app.routers import devices as devices_module

DEVICE_ID = "gw-test-switch-concurrency"
TEST_URL = os.getenv("IOT_TEST_MYSQL_URL")


class OrderedFakeMQTT:
    def __init__(self) -> None:
        self.first_started = asyncio.Event()
        self.release_first = asyncio.Event()
        self.order: list[str] = []

    async def publish_command(
        self,
        device_id: str,
        command: str,
        payload=None,
    ) -> bool:
        self.order.append(command)
        if command == "on":
            self.first_started.set()
            await self.release_first.wait()
        return True


@unittest.skipUnless(TEST_URL, "需要设置 IOT_TEST_MYSQL_URL 才运行 MySQL 集成测试")
class MySQLConcurrencyTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        assert TEST_URL is not None
        self.engine = create_async_engine(TEST_URL, pool_pre_ping=True)
        self.session_factory = async_sessionmaker(
            self.engine,
            class_=AsyncSession,
            expire_on_commit=False,
        )
        async with self.engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)

        async with self.session_factory() as session:
            from app.models.database import Device

            device = await session.get(Device, DEVICE_ID)
            if device is None:
                session.add(
                    Device(
                        id=DEVICE_ID,
                        name="并发测试设备",
                        type="sensor",
                        status="online",
                    )
                )
                await session.commit()

        self.fake_mqtt = OrderedFakeMQTT()
        self.old_mqtt_service = devices_module.mqtt_service
        devices_module.mqtt_service = self.fake_mqtt

    async def asyncTearDown(self) -> None:
        devices_module.mqtt_service = self.old_mqtt_service
        async with self.engine.begin() as conn:
            await conn.run_sync(
                lambda sync_conn: sync_conn.execute(
                    text("DELETE FROM commands WHERE device_id = :device_id"),
                    {"device_id": DEVICE_ID},
                )
            )
            await conn.run_sync(
                lambda sync_conn: sync_conn.execute(
                    text("DELETE FROM devices WHERE id = :device_id"),
                    {"device_id": DEVICE_ID},
                )
            )
        await self.engine.dispose()

    async def call_api(self, command: str):
        async with self.session_factory() as session:
            return await send_command(
                DEVICE_ID,
                CommandRequest(command=command),
                session=session,
            )

    async def test_switch_commands_serialize_without_deadlock(self):
        first_task = asyncio.create_task(self.call_api("on"))
        await asyncio.wait_for(self.fake_mqtt.first_started.wait(), timeout=5)

        second_task = asyncio.create_task(self.call_api("off"))
        # 给第二个请求时间到达设备行锁；不能让它先误确认。
        await asyncio.sleep(0.5)
        self.fake_mqtt.release_first.set()

        first_result, second_result = await asyncio.gather(
            first_task, second_task
        )

        self.assertEqual(self.fake_mqtt.order, ["on", "off"])
        self.assertEqual(first_result.status, "sent")
        self.assertEqual(second_result.status, "sent")

        async with self.session_factory() as session:
            result = await session.execute(
                Command.__table__.select()
                .where(Command.device_id == DEVICE_ID)
                .order_by(Command.id)
            )
            rows = result.all()

        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0].command, "on")
        self.assertEqual(rows[0].status, "superseded")
        self.assertEqual(rows[1].command, "off")
        self.assertEqual(rows[1].status, "sent")
        self.assertLess(rows[0].sent_at, rows[1].sent_at)


if __name__ == "__main__":
    unittest.main()
