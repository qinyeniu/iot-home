"""命令状态闭环：根据节点回报的 OnOff 状态确认开关命令。"""

from datetime import datetime, timedelta
from typing import Optional

from sqlalchemy import select, update
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import settings
from app.models.database import Command, Metric

SWITCH_COMMANDS = ("on", "off", "toggle")


def expected_switch_state(command: str, previous_state: Optional[int]) -> Optional[int]:
    """返回命令执行后应出现的开关状态；无法推断时返回 None。"""
    if command == "on":
        return 1
    if command == "off":
        return 0
    if command == "toggle":
        if previous_state is None:
            return None
        return 0 if previous_state else 1
    return None


async def supersede_open_switch_commands(
    session: AsyncSession,
    device_id: str,
    current_command_id: int,
) -> int:
    """新开关命令发出前，关闭该设备此前未完成的开关命令。"""
    result = await session.execute(
        update(Command)
        .where(
            Command.device_id == device_id,
            Command.id != current_command_id,
            Command.status.in_(("pending", "sent")),
            Command.command.in_(SWITCH_COMMANDS),
        )
        .values(status="superseded")
    )
    return result.rowcount or 0


async def _previous_on_off(
    session: AsyncSession,
    device_id: str,
    sent_at: datetime,
) -> Optional[int]:
    """读取一条开关命令发送前最近一次已知状态。"""
    result = await session.execute(
        select(Metric.value)
        .where(
            Metric.device_id == device_id,
            Metric.metric == "on_off",
            Metric.received_at.is_not(None),
            Metric.received_at < sent_at,
        )
        .order_by(Metric.received_at.desc(), Metric.id.desc())
        .limit(1)
    )
    value = result.scalar_one_or_none()
    if value is None:
        return None
    return 1 if float(value) >= 0.5 else 0


async def acknowledge_switch_command(
    session: AsyncSession,
    device_id: str,
    observed_state: int,
    observed_at: datetime,
) -> Optional[Command]:
    """根据一条 on_off 遥测确认一个未完成开关命令。

    业务层保证同一设备同一时刻只有一个未完成开关命令。若数据库中仍有
    多个候选（例如旧版本遗留），不猜测回报归属，宁可不确认。
    """
    if observed_state not in (0, 1):
        return None

    window_start = observed_at - timedelta(
        seconds=settings.COMMAND_ACK_WINDOW_SECONDS
    )
    result = await session.execute(
        select(Command)
        .where(
            Command.device_id == device_id,
            Command.status == "sent",
            Command.sent_at.is_not(None),
            Command.sent_at >= window_start,
            Command.sent_at <= observed_at,
            Command.command.in_(SWITCH_COMMANDS),
        )
        .order_by(Command.sent_at.desc(), Command.id.desc())
        .limit(2)
    )
    candidates = list(result.scalars().all())

    # 多候选意味着无法仅用 0/1 最终状态建立因果关联。
    if len(candidates) != 1:
        return None

    command = candidates[0]
    previous_state: Optional[int] = None
    if command.command == "toggle":
        assert command.sent_at is not None
        previous_state = await _previous_on_off(
            session, device_id, command.sent_at
        )

    expected_state = expected_switch_state(
        command.command, previous_state
    )
    if expected_state is None or expected_state != observed_state:
        return None

    # 条件更新防止与超时任务或未来的多副本消费者发生状态覆盖。
    update_result = await session.execute(
        update(Command)
        .where(
            Command.id == command.id,
            Command.status == "sent",
        )
        .values(
            status="acknowledged",
            acknowledged_at=observed_at,
        )
    )
    if update_result.rowcount != 1:
        return None

    command.status = "acknowledged"
    command.acknowledged_at = observed_at
    return command
