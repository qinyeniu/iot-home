"""
MQTT 客户端服务
订阅遥测数据和设备状态
"""

import asyncio
import json
import logging
import math
import re
from datetime import datetime
from typing import Any, Optional
import aiomqtt
from sqlalchemy.ext.asyncio import AsyncSession
from app.config import settings
from app.services.topics import device_command_topic, split_device_id
from app.services.command_ack import acknowledge_switch_command
from app.models.session import async_session_factory
from app.models.database import Device, Metric

logger = logging.getLogger(__name__)


class MQTTService:
    """MQTT 服务类"""
    
    def __init__(self):
        self.client: Optional[aiomqtt.Client] = None
        self._running = False
    
    async def start(self):
        """启动 MQTT 客户端"""
        self._running = True
        logger.info(f"连接 MQTT: {settings.MQTT_HOST}:{settings.MQTT_PORT}")
        
        while self._running:
            try:
                async with aiomqtt.Client(
                    hostname=settings.MQTT_HOST,
                    port=settings.MQTT_PORT,
                    username=settings.MQTT_USER,
                    password=settings.MQTT_PASSWORD,
                    keepalive=60
                ) as client:
                    self.client = client
                    logger.info("MQTT 连接成功")
                    
                    # 订阅主题
                    await client.subscribe(settings.mqtt_topic_telemetry)
                    await client.subscribe(settings.mqtt_topic_status)
                    logger.info(f"已订阅: {settings.mqtt_topic_telemetry}")
                    logger.info(f"已订阅: {settings.mqtt_topic_status}")
                    
                    # 处理消息
                    async for message in client.messages:
                        await self._handle_message(message)
                        
            except aiomqtt.MqttError as e:
                if not self._running:
                    logger.info("MQTT disconnected during shutdown")
                    break
                logger.warning(f"MQTT 连接断开: {e}，5秒后重连...")
                await asyncio.sleep(5)
            except Exception as e:
                logger.error(f"MQTT 错误: {e}，5秒后重连...")
                await asyncio.sleep(5)
    
    async def stop(self):
        """停止 MQTT 客户端"""
        self._running = False
        client = self.client
        self.client = None
        if client is not None:
            disconnect = getattr(client, "disconnect", None)
            if asyncio.iscoroutinefunction(disconnect):
                # Future aiomqtt versions may expose an async disconnect().
                await disconnect()
            else:
                # aiomqtt 2.0.x disconnects when leaving its async context.
                # Close the wrapped client so the blocked message loop exits.
                underlying = getattr(client, "_client", None)
                if underlying is not None:
                    underlying.disconnect()
        logger.info("MQTT 客户端已停止")
    
    async def _handle_message(self, message: aiomqtt.Message):
        """处理 MQTT 消息"""
        try:
            topic = str(message.topic)
            payload = json.loads(message.payload.decode())
            if not isinstance(payload, dict):
                logger.warning("忽略非对象 MQTT 消息: %s", topic)
                return

            logger.debug(f"收到消息: {topic} -> {payload}")

            # 解析主题：iot-home/{gateway_id}/nodes/{node_id}/telemetry
            parts = topic.split("/")
            if len(parts) == 5:
                gateway_id = parts[1]
                node_id = parts[3]
                msg_type = parts[4]
                device_id = f"{gateway_id}-{node_id}"
                target = split_device_id(device_id)

                if target is None or target[0] != gateway_id:
                    logger.warning("忽略无法识别的设备主题: %s", topic)
                    return

                if msg_type == "telemetry":
                    await self._handle_telemetry(gateway_id, node_id, payload)
                elif msg_type == "status":
                    await self._handle_status(gateway_id, node_id, payload)
                    
        except json.JSONDecodeError:
            logger.warning(f"无效的 JSON: {message.payload}")
        except Exception as e:
            logger.error(f"处理消息失败: {e}")
    
    async def _handle_telemetry(
        self,
        gateway_id: str,
        node_id: str,
        payload: dict[str, Any],
    ) -> None:
        """处理遥测数据"""
        device_id = f"{gateway_id}-{node_id}"
        
        async with async_session_factory() as session:
            try:
                # 确保设备存在；遥测本身也证明设备当前在线
                device = await self._ensure_device(
                    session, device_id, node_id, "sensor", gateway_id
                )
                device.status = "online"
                device.last_seen = datetime.now()
                
                # 指标可保留设备时间；命令确认以服务端实际接收时间为准。
                try:
                    metric_ts = datetime.fromisoformat(
                        payload.get("ts", datetime.now().isoformat())
                    )
                    if metric_ts.tzinfo is not None:
                        metric_ts = metric_ts.astimezone().replace(tzinfo=None)
                except (TypeError, ValueError):
                    logger.warning("设备上报了无效时间戳，改用服务端时间: %s", device_id)
                    metric_ts = datetime.now()
                received_at = datetime.now()

                data = payload.get("data", {})
                if not isinstance(data, dict):
                    logger.warning("设备上报 data 不是对象，忽略指标: %s", device_id)
                    data = {}

                saved_metrics: list[str] = []
                for metric, value in data.items():
                    if (
                        not isinstance(metric, str)
                        or re.fullmatch(r"[A-Za-z0-9_.:-]{1,64}", metric) is None
                    ):
                        logger.warning("忽略非法指标名: %s", metric)
                        continue
                    if not (
                        isinstance(value, (int, float)) and math.isfinite(value)
                    ):
                        logger.warning("忽略非数值指标 %s: %s", metric, device_id)
                        continue

                    metric_record = Metric(
                        device_id=device_id,
                        metric=metric,
                        value=float(value),
                        ts=metric_ts,
                        received_at=received_at,
                    )
                    session.add(metric_record)
                    saved_metrics.append(metric)

                on_off = data.get("on_off")
                if (
                    isinstance(on_off, (int, float))
                    and math.isfinite(on_off)
                    and float(on_off) in (0.0, 1.0)
                ):
                    # flush 当前指标；状态归属只查询服务端接收时间早于命令的记录。
                    await session.flush()
                    await acknowledge_switch_command(
                        session, device_id, int(on_off), received_at
                    )

                await session.commit()
                logger.info(f"遥测数据已保存: {device_id} - {saved_metrics}")
                
            except Exception as e:
                await session.rollback()
                logger.error(f"保存遥测数据失败: {e}")
    
    async def _handle_status(
        self,
        gateway_id: str,
        node_id: str,
        payload: dict[str, Any],
    ) -> None:
        """处理设备状态"""
        device_id = f"{gateway_id}-{node_id}"
        status = payload.get("status", "unknown")
        if status not in {"online", "offline", "unknown"}:
            status = "unknown"
        
        async with async_session_factory() as session:
            try:
                # 更新设备状态
                device = await session.get(Device, device_id)
                if device:
                    device.status = status
                    device.last_seen = datetime.now()
                    await session.commit()
                    logger.info(f"设备状态已更新: {device_id} -> {status}")
                else:
                    logger.warning(f"设备不存在: {device_id}")
                    
            except Exception as e:
                await session.rollback()
                logger.error(f"更新设备状态失败: {e}")
    
    async def _ensure_device(
        self,
        session: AsyncSession,
        device_id: str,
        name: str,
        device_type: str,
        parent_id: str,
    ) -> Device:
        """确保设备存在，并返回设备实例"""
        device = await session.get(Device, device_id)
        if not device:
            device = Device(
                id=device_id,
                name=name,
                type=device_type,
                parent_id=parent_id,
                status="online",
                last_seen=datetime.now()
            )
            session.add(device)
            await session.flush()
            logger.info(f"新设备已注册: {device_id}")
        return device
    
    async def publish_command(
        self,
        device_id: str,
        command: str,
        payload: Optional[dict[str, Any]] = None,
    ) -> bool:
        """发布命令到设备"""
        if not self.client:
            logger.error("MQTT 客户端未连接")
            return False
        
        try:
            topic = device_command_topic(settings.MQTT_TOPIC_PREFIX, device_id)
            if topic is None:
                logger.error(f"无效的设备ID: {device_id}")
                return False
            
            message = {
                "command": command,
                "payload": payload or {},
                "ts": datetime.now().isoformat()
            }
            
            await self.client.publish(topic, json.dumps(message).encode())
            logger.info(f"命令已发送: {device_id} -> {command}")
            return True
            
        except Exception as e:
            logger.error(f"发送命令失败: {e}")
            return False


# 全局 MQTT 服务实例
mqtt_service = MQTTService()
