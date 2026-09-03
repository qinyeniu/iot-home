"""
设备 API 路由
提供设备查询、指标查询、命令下发等接口
"""

from datetime import datetime, timedelta
from typing import List, Optional
from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import select, func, and_
from sqlalchemy.ext.asyncio import AsyncSession
from app.models.session import get_session
from app.models.database import Device, Metric, Command
from app.services.mqtt import mqtt_service
from pydantic import BaseModel

router = APIRouter(prefix="/api", tags=["设备管理"])


# Pydantic 模型
class DeviceResponse(BaseModel):
    id: str
    name: str
    type: str
    parent_id: Optional[str]
    status: str
    last_seen: Optional[datetime]
    created_at: datetime
    
    class Config:
        from_attributes = True


class MetricResponse(BaseModel):
    device_id: str
    metric: str
    value: float
    ts: datetime


class CommandRequest(BaseModel):
    command: str
    payload: Optional[dict] = None


class CommandResponse(BaseModel):
    id: int
    device_id: str
    command: str
    payload: Optional[dict]
    status: str
    created_at: datetime


@router.get("/devices", response_model=List[DeviceResponse])
async def get_devices(
    type: Optional[str] = Query(None, description="设备类型筛选"),
    status: Optional[str] = Query(None, description="设备状态筛选"),
    session: AsyncSession = Depends(get_session)
):
    """获取设备列表"""
    query = select(Device)
    
    if type:
        query = query.where(Device.type == type)
    if status:
        query = query.where(Device.status == status)
    
    query = query.order_by(Device.created_at.desc())
    result = await session.execute(query)
    return result.scalars().all()


@router.get("/devices/{device_id}", response_model=DeviceResponse)
async def get_device(
    device_id: str,
    session: AsyncSession = Depends(get_session)
):
    """获取单个设备详情"""
    device = await session.get(Device, device_id)
    if not device:
        raise HTTPException(status_code=404, detail="设备不存在")
    return device


@router.get("/devices/{device_id}/metrics")
async def get_device_metrics(
    device_id: str,
    metric: Optional[str] = Query(None, description="指标名称"),
    hours: int = Query(24, description="查询时间范围（小时）"),
    limit: int = Query(1000, description="返回条数限制"),
    session: AsyncSession = Depends(get_session)
):
    """获取设备指标数据"""
    # 检查设备是否存在
    device = await session.get(Device, device_id)
    if not device:
        raise HTTPException(status_code=404, detail="设备不存在")
    
    # 构建查询
    since = datetime.now() - timedelta(hours=hours)
    query = select(Metric).where(
        and_(
            Metric.device_id == device_id,
            Metric.ts >= since
        )
    )
    
    if metric:
        query = query.where(Metric.metric == metric)
    
    query = query.order_by(Metric.ts.desc()).limit(limit)
    result = await session.execute(query)
    return result.scalars().all()


@router.get("/metrics/latest")
async def get_latest_metrics(
    device_id: Optional[str] = Query(None, description="设备ID"),
    metric: Optional[str] = Query(None, description="指标名称"),
    session: AsyncSession = Depends(get_session)
):
    """获取最新指标数据"""
    # 使用子查询获取每个设备每个指标的最新值
    subq = (
        select(
            Metric.device_id,
            Metric.metric,
            func.max(Metric.ts).label("max_ts")
        )
        .group_by(Metric.device_id, Metric.metric)
    )
    
    if device_id:
        subq = subq.where(Metric.device_id == device_id)
    if metric:
        subq = subq.where(Metric.metric == metric)
    
    subq = subq.subquery()
    
    query = (
        select(Metric)
        .join(
            subq,
            and_(
                Metric.device_id == subq.c.device_id,
                Metric.metric == subq.c.metric,
                Metric.ts == subq.c.max_ts
            )
        )
    )
    
    result = await session.execute(query)
    return result.scalars().all()


@router.post("/devices/{device_id}/commands", response_model=CommandResponse)
async def send_command(
    device_id: str,
    request: CommandRequest,
    session: AsyncSession = Depends(get_session)
):
    """发送命令到设备"""
    # 检查设备是否存在
    device = await session.get(Device, device_id)
    if not device:
        raise HTTPException(status_code=404, detail="设备不存在")
    
    # 创建命令记录
    command = Command(
        device_id=device_id,
        command=request.command,
        payload=request.payload,
        status="pending"
    )
    session.add(command)
    await session.commit()
    await session.refresh(command)
    
    # 通过 MQTT 发送命令
    success = await mqtt_service.publish_command(device_id, request.command, request.payload)
    
    if success:
        command.status = "sent"
        command.sent_at = datetime.now()
        await session.commit()
    else:
        command.status = "failed"
        await session.commit()
        raise HTTPException(status_code=500, detail="命令发送失败")
    
    return command


@router.get("/commands", response_model=List[CommandResponse])
async def get_commands(
    device_id: Optional[str] = Query(None, description="设备ID"),
    status: Optional[str] = Query(None, description="命令状态"),
    limit: int = Query(50, description="返回条数限制"),
    session: AsyncSession = Depends(get_session)
):
    """获取命令历史"""
    query = select(Command)
    
    if device_id:
        query = query.where(Command.device_id == device_id)
    if status:
        query = query.where(Command.status == status)
    
    query = query.order_by(Command.created_at.desc()).limit(limit)
    result = await session.execute(query)
    return result.scalars().all()


@router.get("/health")
async def health_check():
    """健康检查接口"""
    return {
        "status": "healthy",
        "timestamp": datetime.now().isoformat(),
        "mqtt_connected": mqtt_service.client is not None
    }
