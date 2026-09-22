"""
数据库模型定义
使用 SQLAlchemy 2.0 异步模式
"""

from datetime import datetime
from typing import Optional
from sqlalchemy import Column, String, DateTime, Enum, BigInteger, Float, Integer, JSON, ForeignKey, Index
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy.sql import func

Base = declarative_base()


class Device(Base):
    """设备表"""
    __tablename__ = "devices"
    
    id = Column(String(64), primary_key=True, comment="设备唯一标识")
    name = Column(String(128), nullable=False, comment="设备名称")
    type = Column(Enum("gateway", "sensor", "switch"), nullable=False, comment="设备类型")
    parent_id = Column(String(64), ForeignKey("devices.id", ondelete="CASCADE"), nullable=True, comment="父设备ID")
    status = Column(Enum("online", "offline", "unknown"), default="unknown", comment="设备状态")
    last_seen = Column(DateTime, nullable=True, comment="最后在线时间")
    created_at = Column(DateTime, default=func.now(), comment="创建时间")
    updated_at = Column(DateTime, default=func.now(), onupdate=func.now(), comment="更新时间")
    
    __table_args__ = (
        Index("idx_type", "type"),
        Index("idx_parent_id", "parent_id"),
        Index("idx_status", "status"),
        Index("idx_last_seen", "last_seen"),
        {"comment": "设备信息表"}
    )


class Metric(Base):
    """指标表"""
    __tablename__ = "metrics"
    
    id = Column(BigInteger().with_variant(Integer, "sqlite"), primary_key=True, autoincrement=True)
    device_id = Column(String(64), ForeignKey("devices.id", ondelete="CASCADE"), nullable=False, comment="设备ID")
    metric = Column(String(64), nullable=False, comment="指标名称")
    value = Column(Float, nullable=False, comment="指标值")
    ts = Column(DateTime(3), nullable=False, comment="设备时间戳（毫秒精度）")
    received_at = Column(DateTime(3), nullable=False, comment="服务端实际接收时间")
    created_at = Column(DateTime, default=func.now(), comment="记录创建时间")
    
    __table_args__ = (
        Index("idx_device_metric", "device_id", "metric"),
        Index("idx_ts", "ts"),
        Index("idx_device_ts", "device_id", "ts"),
        Index("idx_metric_history", "device_id", "metric", "received_at", "id"),
        {"comment": "指标数据表"}
    )


class Command(Base):
    """命令表"""
    __tablename__ = "commands"
    
    id = Column(BigInteger().with_variant(Integer, "sqlite"), primary_key=True, autoincrement=True)
    device_id = Column(String(64), ForeignKey("devices.id", ondelete="CASCADE"), nullable=False, comment="目标设备ID")
    command = Column(String(64), nullable=False, comment="命令名称")
    payload = Column(JSON, nullable=True, comment="命令参数")
    status = Column(Enum("pending", "sent", "acknowledged", "failed", "superseded", "timeout"), default="pending", comment="命令状态")
    created_at = Column(DateTime, default=func.now(), comment="创建时间")
    sent_at = Column(DateTime, nullable=True, comment="发送时间")
    acknowledged_at = Column(DateTime, nullable=True, comment="确认时间")
    
    __table_args__ = (
        Index("idx_device_status", "device_id", "status"),
        Index("idx_created_at", "created_at"),
        Index("idx_command_outbox", "device_id", "status", "sent_at", "id"),
        {"comment": "设备命令表"}
    )


class ProcessedMessage(Base):
    '''Idempotency: a redelivered QoS1 message_id is processed only once.'''
    __tablename__ = "processed_messages"

    message_id = Column(String(80), primary_key=True, comment="unique message id")
    device_id = Column(String(64), nullable=False, comment="source device id")
    processed_at = Column(DateTime(3), nullable=False, comment="first processed time")

    __table_args__ = (
        Index("idx_pm_device_time", "device_id", "processed_at"),
        {"comment": "message idempotency table"},
    )
