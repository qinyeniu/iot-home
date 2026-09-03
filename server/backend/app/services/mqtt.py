"""
MQTT 客户端服务
订阅遥测数据和设备状态
"""

import asyncio
import json
import logging
from datetime import datetime
from typing import Optional
import aiomqtt
from app.config import settings
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
                logger.warning(f"MQTT 连接断开: {e}，5秒后重连...")
                await asyncio.sleep(5)
            except Exception as e:
                logger.error(f"MQTT 错误: {e}，5秒后重连...")
                await asyncio.sleep(5)
    
    async def stop(self):
        """停止 MQTT 客户端"""
        self._running = False
        if self.client:
            await self.client.disconnect()
        logger.info("MQTT 客户端已停止")
    
    async def _handle_message(self, message: aiomqtt.Message):
        """处理 MQTT 消息"""
        try:
            topic = str(message.topic)
            payload = json.loads(message.payload.decode())
            
            logger.debug(f"收到消息: {topic} -> {payload}")
            
            # 解析主题：iot-home/{gateway_id}/nodes/{node_id}/telemetry
            parts = topic.split("/")
            if len(parts) >= 5:
                gateway_id = parts[1]
                node_id = parts[3]
                msg_type = parts[4]
                
                if msg_type == "telemetry":
                    await self._handle_telemetry(gateway_id, node_id, payload)
                elif msg_type == "status":
                    await self._handle_status(gateway_id, node_id, payload)
                    
        except json.JSONDecodeError:
            logger.warning(f"无效的 JSON: {message.payload}")
        except Exception as e:
            logger.error(f"处理消息失败: {e}")
    
    async def _handle_telemetry(self, gateway_id: str, node_id: str, payload: dict):
        """处理遥测数据"""
        device_id = f"{gateway_id}-{node_id}"
        
        async with async_session_factory() as session:
            try:
                # 确保设备存在
                await self._ensure_device(session, device_id, node_id, "sensor", gateway_id)
                
                # 写入指标数据
                ts = datetime.fromisoformat(payload.get("ts", datetime.now().isoformat()))
                
                for metric, value in payload.get("data", {}).items():
                    if isinstance(value, (int, float)):
                        metric_record = Metric(
                            device_id=device_id,
                            metric=metric,
                            value=float(value),
                            ts=ts
                        )
                        session.add(metric_record)
                
                await session.commit()
                logger.info(f"遥测数据已保存: {device_id} - {list(payload.get('data', {}).keys())}")
                
            except Exception as e:
                await session.rollback()
                logger.error(f"保存遥测数据失败: {e}")
    
    async def _handle_status(self, gateway_id: str, node_id: str, payload: dict):
        """处理设备状态"""
        device_id = f"{gateway_id}-{node_id}"
        status = payload.get("status", "unknown")
        
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
    
    async def _ensure_device(self, session, device_id: str, name: str, device_type: str, parent_id: str):
        """确保设备存在"""
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
    
    async def publish_command(self, device_id: str, command: str, payload: dict = None):
        """发布命令到设备"""
        if not self.client:
            logger.error("MQTT 客户端未连接")
            return False
        
        try:
            # 命令主题：iot-home/{gateway_id}/nodes/{node_id}/cmd
            parts = device_id.split("-", 1)
            if len(parts) != 2:
                logger.error(f"无效的设备ID: {device_id}")
                return False
            
            gateway_id, node_id = parts
            topic = f"{settings.MQTT_TOPIC_PREFIX}/{gateway_id}/nodes/{node_id}/cmd"
            
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
