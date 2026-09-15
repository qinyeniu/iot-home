"""
设备在线状态维护。

MQTT retained status 可以在服务重启后快速恢复 last known state，但不能把
历史上的 online 永久当成当前状态。后台任务会把超过阈值没有遥测或状态心跳的
设备标记为 offline；设备下一次上报时 MQTT 处理逻辑会立即改回 online。
"""

import asyncio
import logging
from datetime import datetime, timedelta

from sqlalchemy import update

from app.config import settings
from app.models.database import Device
from app.models.session import async_session_factory

logger = logging.getLogger(__name__)


async def mark_stale_devices_offline() -> int:
    """将超时未上报的在线设备标记为离线，返回本次变更数量。"""
    cutoff = datetime.now() - timedelta(
        seconds=settings.DEVICE_OFFLINE_TIMEOUT_SECONDS
    )

    async with async_session_factory() as session:
        result = await session.execute(
            update(Device)
            .where(Device.status == "online")
            .where(Device.last_seen.is_not(None))
            .where(Device.last_seen < cutoff)
            .values(status="offline")
        )
        await session.commit()

    changed = result.rowcount or 0
    if changed:
        logger.warning(
            "设备超时离线检查：%d 台设备超过 %d 秒未上报，已标记为 offline",
            changed,
            settings.DEVICE_OFFLINE_TIMEOUT_SECONDS,
        )
    return changed


async def device_status_monitor() -> None:
    """周期性维护设备在线状态。"""
    interval = max(5, settings.DEVICE_STATUS_CHECK_INTERVAL_SECONDS)
    logger.info(
        "设备状态检查已启动：离线超时=%ds，检查间隔=%ds",
        settings.DEVICE_OFFLINE_TIMEOUT_SECONDS,
        interval,
    )

    while True:
        await asyncio.sleep(interval)
        try:
            await mark_stale_devices_offline()
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            logger.exception("设备超时离线检查失败: %s", exc)