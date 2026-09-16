import asyncio
import unittest

from app.config import settings
from app.services import device_status


class FakeResult:
    def __init__(self, rowcount):
        self.rowcount = rowcount


class FakeSession:
    def __init__(self, rowcount):
        self.rowcount = rowcount
        self.committed = False
        self.executed = 0

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        return False

    async def execute(self, statement):
        self.executed += 1
        return FakeResult(self.rowcount)

    async def commit(self):
        self.committed = True


class FakeSessionFactory:
    def __init__(self, session):
        self.session = session

    def __call__(self):
        return self.session


class DeviceStatusTests(unittest.TestCase):
    def test_stale_online_device_marked_offline(self):
        session = FakeSession(rowcount=1)
        old_factory = device_status.async_session_factory
        old_timeout = settings.DEVICE_OFFLINE_TIMEOUT_SECONDS
        device_status.async_session_factory = FakeSessionFactory(session)
        settings.DEVICE_OFFLINE_TIMEOUT_SECONDS = 180
        try:
            changed = asyncio.run(device_status.mark_stale_devices_offline())
        finally:
            device_status.async_session_factory = old_factory
            settings.DEVICE_OFFLINE_TIMEOUT_SECONDS = old_timeout

        self.assertEqual(changed, 1)
        self.assertEqual(session.executed, 1)
        self.assertTrue(session.committed)

    def test_no_changes_returns_zero(self):
        session = FakeSession(rowcount=0)
        old_factory = device_status.async_session_factory
        device_status.async_session_factory = FakeSessionFactory(session)
        try:
            changed = asyncio.run(device_status.mark_stale_devices_offline())
        finally:
            device_status.async_session_factory = old_factory
        self.assertEqual(changed, 0)


if __name__ == "__main__":
    unittest.main()
