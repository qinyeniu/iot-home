"""设备能力推导。

组合设备（如"传感器 + 开关"）不适合用单一 type 表达，因此能力直接从
该设备近期实际上报过的指标动态推导，无需额外字段或迁移：

    {"sensors": ["temperature", "humidity", "lux"], "switch": true}

只统计最近 ``CAPABILITY_WINDOW_DAYS`` 天内出现过的指标，长期停用的
能力会自动从结果中消失。
"""

from datetime import datetime, timedelta
from typing import Iterable

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.database import Metric

# 被识别为开关能力的指标。
SWITCH_METRICS = frozenset({"on_off"})

# 多久内上报过才算具备该能力。
CAPABILITY_WINDOW_DAYS = 7


async def get_capabilities_map(
    session: AsyncSession,
    device_ids: Iterable[str],
    now: datetime | None = None,
) -> dict[str, dict]:
    """批量返回多个设备的能力，一次分组查询完成。"""
    ids = list(device_ids)
    result: dict[str, dict] = {
        device_id: {"sensors": [], "switch": False} for device_id in ids
    }
    if not ids:
        return result

    reference = now or datetime.now()
    # 优先用服务端接收时间；旧数据可能没有该字段时由调用方保证已迁移。
    since = reference - timedelta(days=CAPABILITY_WINDOW_DAYS)
    statement = (
        select(Metric.device_id, Metric.metric)
        .where(Metric.device_id.in_(ids))
        .where(Metric.received_at >= since)
        .group_by(Metric.device_id, Metric.metric)
    )
    rows = (await session.execute(statement)).all()

    for device_id, metric in rows:
        cap = result[device_id]
        if metric in SWITCH_METRICS:
            cap["switch"] = True
        else:
            cap["sensors"].append(metric)

    for cap in result.values():
        cap["sensors"].sort()
    return result


async def get_capabilities(
    session: AsyncSession,
    device_id: str,
    now: datetime | None = None,
) -> dict:
    """返回单个设备的能力。"""
    return (
        await get_capabilities_map(session, [device_id], now=now)
    )[device_id]
