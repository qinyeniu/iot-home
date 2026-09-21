"""开关命令等待节点回报的超时收敛。"""

import asyncio
import logging
from datetime import datetime, timedelta
from typing import TYPE_CHECKING, Optional

from sqlalchemy import update

from app.config import settings
from app.models.database import Command

if TYPE_CHECKING:
    from sqlalchemy.ext.asyncio import AsyncSession

logger = logging.getLogger(__name__)

# 运行时再导入，避免单元测试仅替换工厂也必须安装 MySQL 驱动。
async_session_factory = None


async def mark_overdue_commands_timeout(
    session: Optional["AsyncSession"] = None,
    now: Optional[datetime] = None,
) -> int:
    """把超过确认时限的 sent 命令标记为 timeout。"""
    observed_at = now or datetime.now()
    cutoff = observed_at - timedelta(
        seconds=settings.COMMAND_TIMEOUT_SECONDS
    )
    statement = (
        update(Command)
        .where(Command.status == "sent")
        .where(Command.sent_at.is_not(None))
        .where(Command.sent_at <= cutoff)
        .values(status="timeout")
    )

    async def execute_with(active_session: "AsyncSession") -> int:
        result = await active_session.execute(statement)
        return result.rowcount or 0

    if session is not None:
        return await execute_with(session)

    factory = async_session_factory
    if factory is None:
        from app.models.session import async_session_factory as factory

    async with factory() as owned_session:
        changed = await execute_with(owned_session)
        await owned_session.commit()
        return changed


async def command_timeout_monitor() -> None:
    """周期收敛超时命令。"""
    interval = max(5, settings.COMMAND_TIMEOUT_CHECK_INTERVAL_SECONDS)
    logger.info(
        "命令超时检查已启动：超时=%ds，检查间隔=%ds",
        settings.COMMAND_TIMEOUT_SECONDS,
        interval,
    )

    while True:
        try:
            changed = await mark_overdue_commands_timeout()
            if changed:
                logger.warning("命令超时收敛：%d 条命令已标记为 timeout", changed)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            logger.exception("命令超时检查失败: %s", exc)
        await asyncio.sleep(interval)
