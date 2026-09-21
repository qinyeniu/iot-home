import unittest
from datetime import datetime, timedelta

from sqlalchemy import select
from sqlalchemy.ext.asyncio import (
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)
from sqlalchemy.pool import StaticPool

from app.models.database import Base, Command, Device, Metric
from app.services import command_timeout
from app.services.command_ack import (
    _previous_on_off,
    acknowledge_switch_command,
    expected_switch_state,
    supersede_open_switch_commands,
)

DEVICE_ID = "gw-001-zb-82cb"


class CommandAckLogicTests(unittest.TestCase):
    def test_expected_switch_state(self):
        self.assertEqual(expected_switch_state("on", 0), 1)
        self.assertEqual(expected_switch_state("on", 1), 1)
        self.assertEqual(expected_switch_state("off", 0), 0)
        self.assertEqual(expected_switch_state("off", 1), 0)
        self.assertEqual(expected_switch_state("toggle", 0), 1)
        self.assertEqual(expected_switch_state("toggle", 1), 0)
        self.assertIsNone(expected_switch_state("toggle", None))
        self.assertIsNone(expected_switch_state("unsupported", 0))


class AsyncCommandAckTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.engine = create_async_engine(
            "sqlite+aiosqlite:///:memory:",
            poolclass=StaticPool,
        )
        async with self.engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)

        self.session = AsyncSession(self.engine)
        self.session.add(
            Device(
                id=DEVICE_ID,
                name="zb-82cb",
                type="sensor",
                status="online",
            )
        )
        await self.session.commit()
        self.now = datetime(2026, 9, 22, 0, 40, 0)
        self.old_timeout_factory = command_timeout.async_session_factory

    async def asyncTearDown(self):
        command_timeout.async_session_factory = self.old_timeout_factory
        await self.session.aclose()
        await self.engine.dispose()

    async def add_command(
        self,
        command: str,
        sent_at,
        status: str = "sent",
    ) -> Command:
        record = Command(
            device_id=DEVICE_ID,
            command=command,
            payload={},
            status=status,
            sent_at=sent_at,
        )
        self.session.add(record)
        await self.session.commit()
        await self.session.refresh(record)
        return record

    async def add_on_off_metric(
        self,
        value: int,
        ts: datetime,
        received_at=None,
    ) -> Metric:
        record = Metric(
            device_id=DEVICE_ID,
            metric="on_off",
            value=value,
            ts=ts,
            received_at=received_at if received_at is not None else ts,
        )
        self.session.add(record)
        await self.session.commit()
        await self.session.refresh(record)
        return record

    async def get_command_status(self, command_id: int) -> str:
        result = await self.session.execute(
            select(Command.status).where(Command.id == command_id)
        )
        return result.scalar_one()

    async def test_acknowledge_on_command(self):
        command = await self.add_command("on", self.now - timedelta(seconds=1))

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNotNone(acknowledged)
        self.assertEqual(
            await self.get_command_status(command.id), "acknowledged"
        )

    async def test_do_not_acknowledge_when_state_mismatches(self):
        command = await self.add_command("off", self.now - timedelta(seconds=1))

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNone(acknowledged)
        self.assertEqual(await self.get_command_status(command.id), "sent")

    async def test_do_not_acknowledge_future_command(self):
        command = await self.add_command(
            "on", self.now + timedelta(milliseconds=1)
        )

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNone(acknowledged)
        self.assertEqual(await self.get_command_status(command.id), "sent")

    async def test_window_boundary_accepts_exactly_30_seconds(self):
        command = await self.add_command(
            "on", self.now - timedelta(seconds=30)
        )

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNotNone(acknowledged)
        self.assertEqual(
            await self.get_command_status(command.id), "acknowledged"
        )

    async def test_window_boundary_rejects_older_command(self):
        command = await self.add_command(
            "on", self.now - timedelta(seconds=30, milliseconds=1)
        )

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNone(acknowledged)
        self.assertEqual(await self.get_command_status(command.id), "sent")

    async def test_toggle_uses_previous_on_off(self):
        await self.add_on_off_metric(
            0,
            self.now - timedelta(seconds=2),
            self.now - timedelta(seconds=2),
        )
        command = await self.add_command(
            "toggle", self.now - timedelta(seconds=1)
        )

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNotNone(acknowledged)
        self.assertEqual(
            await self.get_command_status(command.id), "acknowledged"
        )

    async def test_toggle_without_previous_state_is_not_acknowledged(self):
        command = await self.add_command(
            "toggle", self.now - timedelta(seconds=1)
        )

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNone(acknowledged)
        self.assertEqual(await self.get_command_status(command.id), "sent")

    async def test_toggle_does_not_use_current_metric(self):
        command = await self.add_command(
            "toggle", self.now - timedelta(seconds=1)
        )
        await self.add_on_off_metric(1, self.now, self.now)

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNone(acknowledged)
        self.assertEqual(await self.get_command_status(command.id), "sent")

    async def test_toggle_uses_server_time_not_device_time(self):
        sent_at = self.now - timedelta(seconds=1)
        await self.add_command("toggle", sent_at)
        # 设备时间戳较早，但服务端实际在命令发出后才收到；不能当作前置状态。
        await self.add_on_off_metric(
            1,
            sent_at - timedelta(seconds=1),
            self.now,
        )

        previous_state = await _previous_on_off(
            self.session, DEVICE_ID, sent_at
        )

        self.assertIsNone(previous_state)

    async def test_multiple_candidates_are_not_guessed(self):
        first = await self.add_command("on", self.now - timedelta(seconds=3))
        second = await self.add_command(
            "toggle", self.now - timedelta(seconds=1)
        )

        acknowledged = await acknowledge_switch_command(
            self.session, DEVICE_ID, 1, self.now
        )

        self.assertIsNone(acknowledged)
        self.assertEqual(await self.get_command_status(first.id), "sent")
        self.assertEqual(await self.get_command_status(second.id), "sent")

    async def test_supersede_previous_open_commands(self):
        previous = await self.add_command(
            "on", self.now - timedelta(seconds=2)
        )
        previous_id = previous.id
        current = await self.add_command("off", None, status="pending")
        current_id = current.id

        changed = await supersede_open_switch_commands(
            self.session, DEVICE_ID, current_id
        )

        self.assertEqual(changed, 1)
        await self.session.commit()
        self.assertEqual(
            await self.get_command_status(previous_id), "superseded"
        )
        self.assertEqual(
            await self.get_command_status(current_id), "pending"
        )

    async def test_timeout_marks_old_sent_command_with_provided_session(self):
        old_command = await self.add_command(
            "on", self.now - timedelta(seconds=31)
        )
        old_id = old_command.id
        recent_command = await self.add_command(
            "off", self.now - timedelta(seconds=29)
        )
        recent_id = recent_command.id

        changed = await command_timeout.mark_overdue_commands_timeout(
            self.session, now=self.now
        )

        self.assertEqual(changed, 1)
        await self.session.commit()
        self.assertEqual(
            await self.get_command_status(old_id), "timeout"
        )
        self.assertEqual(
            await self.get_command_status(recent_id), "sent"
        )

    async def test_timeout_marks_command_at_exact_threshold(self):
        command = await self.add_command(
            "on", self.now - timedelta(seconds=30)
        )
        command_id = command.id

        changed = await command_timeout.mark_overdue_commands_timeout(
            self.session, now=self.now
        )

        self.assertEqual(changed, 1)
        await self.session.commit()
        self.assertEqual(
            await self.get_command_status(command_id), "timeout"
        )

    async def test_timeout_owned_session_commits(self):
        old_command = await self.add_command(
            "on", self.now - timedelta(seconds=31)
        )
        old_id = old_command.id
        factory = async_sessionmaker(
            self.engine,
            class_=AsyncSession,
            expire_on_commit=False,
        )
        command_timeout.async_session_factory = factory

        changed = await command_timeout.mark_overdue_commands_timeout(now=self.now)

        self.assertEqual(changed, 1)
        self.assertEqual(
            await self.get_command_status(old_id), "timeout"
        )


if __name__ == "__main__":
    unittest.main()
