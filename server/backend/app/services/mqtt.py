"""
MQTT 客户端服务
- QoS1 订阅遥测/状态，杜绝网络抖动时静默丢消息；
- 网络循环只负责快速排空消息，数据库写入由独立 worker 串行完成，
  保持同一设备的消息顺序，同时避免慢数据库反压到 MQTT 收包循环。
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

# 已解码但尚未写入数据库的消息最大积压量。持续增长说明处理速度跟不上
# （通常是数据库过载），监控应关注对应的积压告警。
MESSAGE_QUEUE_MAXSIZE = 2000


class MQTTService:
    """MQTT 服务类"""

    def __init__(self):
        self.client: Optional[aiomqtt.Client] = None
        self._running = False
        self._message_queue: Optional[asyncio.Queue[aiomqtt.Message]] = None
        self._worker_task: Optional[asyncio.Task] = None

    async def start(self):
        """启动 MQTT 客户端"""
        self._running = True
        self._message_queue = asyncio.Queue(maxsize=MESSAGE_QUEUE_MAXSIZE)
        self._worker_task = asyncio.create_task(self._process_messages())
        logger.info("连接 MQTT: %s:%s", settings.MQTT_HOST, settings.MQTT_PORT)

        try:
            await self._run_client()
        finally:
            # 客户端循环退出（关停或致命错误）时一并收尾处理任务。
            if self._worker_task is not None:
                self._worker_task.cancel()
                try:
                    await self._worker_task
                except asyncio.CancelledError:
                    pass
                self._worker_task = None

    async def _run_client(self):
        while self._running:
            try:
                async with aiomqtt.Client(
                    hostname=settings.MQTT_HOST,
                    port=settings.MQTT_PORT,
                    username=settings.MQTT_USER,
                    password=settings.MQTT_PASSWORD,
                    keepalive=60,
                ) as client:
                    self.client = client
                    logger.info("MQTT 连接成功")

                    # QoS1 订阅：broker 必须收到本进程的 PUBACK 才完成投递；
                    # QoS0 在网络抖动/处理积压时会静默丢消息。
                    await client.subscribe(settings.mqtt_topic_telemetry, qos=1)
                    await client.subscribe(settings.mqtt_topic_status, qos=1)
                    logger.info("已订阅(QoS1): %s", settings.mqtt_topic_telemetry)
                    logger.info("已订阅(QoS1): %s", settings.mqtt_topic_status)

                    # 网络循环只负责快速排空；数据库写入由独立 worker
                    # 串行完成，保持同一设备的消息顺序。
                    async for message in client.messages:
                        assert self._message_queue is not None
                        await self._message_queue.put(message)

            except aiomqtt.MqttError as e:
                if not self._running:
                    logger.info("MQTT disconnected during shutdown")
                    break
                logger.warning("MQTT 连接断开: %s，5秒后重连...", e)
                await asyncio.sleep(5)
            except Exception as e:
                logger.error("MQTT 错误: %s，5秒后重连...", e)
                await asyncio.sleep(5)

    async def _process_messages(self):
        """单 worker 串行处理消息，保证同一设备的命令/状态顺序。"""
        assert self._message_queue is not None
        while True:
            message = await self._message_queue.get()
            backlog = self._message_queue.qsize()
            if backlog >= 50:
                logger.warning(
                    "MQTT 待处理消息积压: %d 条（数据库可能过载）", backlog
                )
            try:
                await self._handle_message(message)
            finally:
                self._message_queue.task_done()

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

            logger.debug("收到消息: %s -> %s", topic, payload)

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
                    await self._handle_status(
                        gateway_id, node_id, payload, message.retain
                    )
        except Exception as e:
            logger.error("处理消息失败: %s", e)

    async def _handle_telemetry(
        self,
        gateway_id: str,
        node_id: str,
        payload: dict[str, Any],
    ) -> None:
        """处理遥测数据：一个事务批量写入本帧全部指标。"""
        device_id = f"{gateway_id}-{node_id}"

        async with async_session_factory() as session:
            try:
                await self._ensure_device(
                    session, device_id, node_id, "sensor", gateway_id
                )

                device = await session.get(Device, device_id)
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
                    logger.warning(
                        "设备上报了无效时间戳，改用服务端时间: %s", device_id
                    )
                    metric_ts = datetime.now()
                received_at = datetime.now()

                data = payload.get("data", {})
                if not isinstance(data, dict):
                    logger.warning(
                        "设备上报 data 不是对象，忽略指标: %s", device_id
                    )
                    data = {}

                metric_records: list[Metric] = []
                saved_metrics: list[str] = []
                for metric, value in data.items():
                    if (
                        not isinstance(metric, str)
                        or re.fullmatch(r"[A-Za-z0-9_.:-]{1,64}", metric) is None
                    ):
                        logger.warning("忽略非法指标名: %s", metric)
                        continue
                    if not (
                        isinstance(value, (int, float))
                        and not isinstance(value, bool)
                        and math.isfinite(value)
                    ):
                        logger.warning(
                            "忽略非数值指标 %s: %s", metric, device_id
                        )
                        continue

                    metric_records.append(
                        Metric(
                            device_id=device_id,
                            metric=metric,
                            value=float(value),
                            ts=metric_ts,
                            received_at=received_at,
                        )
                    )
                    saved_metrics.append(metric)

                if metric_records:
                    # 一次批量 INSERT，避免每条指标一次往返。
                    session.add_all(metric_records)
                    await session.flush()

                # on_off 单独确认在途开关命令；状态归属按 received_at 判断。
                on_off = data.get("on_off")
                if (
                    isinstance(on_off, int)
                    and not isinstance(on_off, bool)
                    and on_off in (0, 1)
                ):
                    await acknowledge_switch_command(
                        session, device_id, on_off, received_at
                    )

                await session.commit()
                logger.info("遥测数据已保存: %s - %s", device_id, saved_metrics)

            except Exception as e:
                await session.rollback()
                logger.error("保存遥测数据失败: %s", e)

    async def _handle_status(
        self,
        gateway_id: str,
        node_id: str,
        payload: dict[str, Any],
        retained: bool = False,
    ) -> None:
        """处理设备状态。

        后端（重新）订阅时 broker 会投递 retained 状态；它可能是很久以前
        的最后状态，不能当作新的心跳刷新 last_seen，否则设备刚掉线时会
        因后端重启被误判在线。非 retained 的状态由设备/网关切实发出，
        才更新心跳时间。
        """
        device_id = f"{gateway_id}-{node_id}"
        status = payload.get("status", "unknown")

        async with async_session_factory() as session:
            try:
                await self._ensure_device(
                    session, device_id, node_id, "sensor", gateway_id
                )
                if status in ("online", "offline"):
                    device = await session.get(Device, device_id)
                    device.status = status
                    if not retained:
                        device.last_seen = datetime.now()
                    await session.commit()
                    logger.info(
                        "设备状态更新: %s -> %s%s",
                        device_id,
                        status,
                        " (retained, last_seen 未更新)" if retained else "",
                    )
            except Exception as e:
                await session.rollback()
                logger.error("更新设备状态失败: %s", e)

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
                status="online",
                parent_id=parent_id,
            )
            session.add(device)
            await session.flush()
            logger.info("新设备已注册: %s", device_id)
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
                logger.error("无效的设备ID: %s", device_id)
                return False

            message = {
                "command": command,
                "payload": payload or {},
                "ts": datetime.now().isoformat(),
            }

            # QoS1：命令至少送达 broker 一次，失败由上层记录为 failed。
            await self.client.publish(topic, json.dumps(message).encode(), qos=1)
            logger.info("命令已发送: %s -> %s", device_id, command)
            return True

        except Exception as e:
            logger.error("发送命令失败: %s", e)
            return False


# 全局 MQTT 服务实例
mqtt_service = MQTTService()
