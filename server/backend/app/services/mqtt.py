"""
MQTT 客户端服务
- QoS1 订阅遥测/状态，减少传输层静默丢消息；
- 网络循环只负责快速排空消息，数据库写入由独立 worker 串行完成，
  保持同一设备的消息顺序，慢数据库只造成短暂积压；
- 数据库瞬时错误有限重试；重试用尽写入本地 dead letter，消息不消失；
- 关停时先排空队列再退出，避免已收消息未落库。
"""

import asyncio
import json
import logging
import math
import os
import re
from datetime import datetime
from typing import Any, Optional

import aiomqtt
from sqlalchemy.exc import SQLAlchemyError
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

# 单条消息遇到数据库瞬时错误时的处理参数。
MAX_PROCESS_ATTEMPTS = 3
PROCESS_RETRY_BASE_DELAY = 0.5

# 关停时最多等待多久让在途消息落库。
SHUTDOWN_DRAIN_TIMEOUT = 20

# 重试用尽的消息落盘位置（容器内默认 /app/dead_letter.jsonl）。
DEAD_LETTER_PATH = os.getenv("IOT_DEAD_LETTER_PATH", "dead_letter.jsonl")


class BadMessageError(ValueError):
    """永久无法处理的消息（格式/主题/data 非法），不重试。"""


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
            await self._shutdown_worker()

    async def _run_client(self):
        while self._running:
            try:
                async with aiomqtt.Client(
                    hostname=settings.MQTT_HOST,
                    port=settings.MQTT_PORT,
                    # 固定 ID + 持久会话：后端短暂重启期间，broker 会替本
                    # 会话排队消息，重连后补发；随机 ID/clean session 会丢弃。
                    identifier="iot-home-backend",
                    protocol=aiomqtt.ProtocolVersion.V311,
                    clean_session=False,
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
            except Exception:
                logger.exception("MQTT 客户端循环异常，5秒后重连...")
                await asyncio.sleep(5)

    async def _process_messages(self):
        """单 worker 串行处理消息，保证同一设备的命令/状态顺序。"""
        assert self._message_queue is not None
        while True:
            message = await self._message_queue.get()
            try:
                await self._process_with_retry(message)
            finally:
                self._message_queue.task_done()

    async def _process_with_retry(self, message: aiomqtt.Message):
        """对数据库瞬时错误做有限退避重试；坏消息不重试。"""
        backlog = self._message_queue.qsize()
        if backlog >= 50:
            logger.warning("MQTT 待处理消息积压: %d 条（数据库可能过载）", backlog)

        delay = PROCESS_RETRY_BASE_DELAY
        for attempt in range(1, MAX_PROCESS_ATTEMPTS + 1):
            try:
                await self._handle_message(message)
                return
            except BadMessageError:
                # 永久坏消息：已在处理处记录，直接丢弃，不重试/不入死信。
                return
            except SQLAlchemyError as exc:
                if attempt >= MAX_PROCESS_ATTEMPTS:
                    logger.error("数据库错误重试 %d 次仍失败", attempt)
                    self._write_dead_letter(message, exc)
                    return
                logger.warning(
                    "数据库瞬时错误(第%d/%d次)，%.1fs 后重试: %s",
                    attempt, MAX_PROCESS_ATTEMPTS, delay, exc,
                )
                await asyncio.sleep(delay)
                delay *= 2
            except Exception as exc:
                # 未预期错误：保留堆栈并落死信，避免毒消息永久阻塞队列。
                logger.exception("处理消息出现未预期错误")
                self._write_dead_letter(message, exc)
                return

    async def _shutdown_worker(self):
        """关停时先排空在途队列，再取消 worker。"""
        if self._message_queue is None or self._worker_task is None:
            return
        try:
            await asyncio.wait_for(
                self._message_queue.join(), timeout=SHUTDOWN_DRAIN_TIMEOUT
            )
        except asyncio.TimeoutError:
            logger.warning(
                "关停 drain 超时(%ds)，仍有消息未落库", SHUTDOWN_DRAIN_TIMEOUT
            )
        self._worker_task.cancel()
        try:
            await self._worker_task
        except asyncio.CancelledError:
            pass
        self._worker_task = None

    def _write_dead_letter(self, message: aiomqtt.Message, exc: Exception):
        entry = {
            "topic": str(message.topic),
            "payload": message.payload.decode(errors="replace"),
            "qos": message.qos,
            "error": repr(exc),
            "failed_at": datetime.now().isoformat(),
        }
        try:
            with open(DEAD_LETTER_PATH, "a", encoding="utf-8") as f:
                f.write(json.dumps(entry, ensure_ascii=False) + "\n")
            logger.error("消息已写入 dead letter：%s（topic=%s）",
                         DEAD_LETTER_PATH, message.topic)
        except OSError:
            logger.exception("无法写入 dead letter 文件：%s", DEAD_LETTER_PATH)

    async def stop(self):
        """停止 MQTT 客户端（FastAPI lifespan 关停时调用）"""
        self._running = False
        client = self.client
        self.client = None
        if client is not None:
            disconnect = getattr(client, "disconnect", None)
            if asyncio.iscoroutinefunction(disconnect):
                await disconnect()
            else:
                # aiomqtt 2.0.x disconnects when leaving its async context.
                underlying = getattr(client, "_client", None)
                if underlying is not None:
                    underlying.disconnect()
        logger.info("MQTT 客户端已停止")

    async def _handle_message(self, message: aiomqtt.Message):
        """处理 MQTT 消息；坏消息抛 BadMessageError，数据库错误向上传播。"""
        topic = str(message.topic)
        try:
            payload = json.loads(message.payload.decode())
        except (json.JSONDecodeError, UnicodeDecodeError):
            logger.warning("忽略无法解析的 MQTT 消息: %s", topic)
            raise BadMessageError("invalid JSON")

        if not isinstance(payload, dict):
            logger.warning("忽略非对象 MQTT 消息: %s", topic)
            raise BadMessageError("payload not an object")

        logger.debug("收到消息: %s -> %s", topic, payload)

        # 主题：iot-home/{gateway_id}/nodes/{node_id}/{telemetry|status}
        parts = topic.split("/")
        if len(parts) != 5:
            logger.warning("忽略主题结构不匹配的消息: %s", topic)
            raise BadMessageError("unexpected topic structure")

        gateway_id, node_id, msg_type = parts[1], parts[3], parts[4]
        device_id = f"{gateway_id}-{node_id}"
        target = split_device_id(device_id)
        if target is None or target[0] != gateway_id:
            logger.warning("忽略无法识别的设备主题: %s", topic)
            raise BadMessageError("unresolvable device topic")

        if msg_type == "telemetry":
            await self._handle_telemetry(
                gateway_id, node_id, payload, message.retain
            )
        elif msg_type == "status":
            await self._handle_status(
                gateway_id, node_id, payload, message.retain
            )
        else:
            logger.warning("忽略未知消息类型 %s: %s", msg_type, topic)
            raise BadMessageError("unknown message type")

    async def _handle_telemetry(
        self,
        gateway_id: str,
        node_id: str,
        payload: dict[str, Any],
        retained: bool = False,
    ) -> None:
        """处理遥测数据：一个事务批量写入本帧全部指标。"""
        device_id = f"{gateway_id}-{node_id}"

        async with async_session_factory() as session:
            await self._ensure_device(
                session, device_id, node_id, "sensor", gateway_id
            )
            device = await session.get(Device, device_id)
            device.status = "online"
            # retained 是 broker 的旧帧，不代表设备此刻在线，不刷新心跳。
            if not retained:
                device.last_seen = datetime.now()

            try:
                metric_ts = datetime.fromisoformat(
                    payload.get("ts", datetime.now().isoformat())
                )
                if metric_ts.tzinfo is not None:
                    metric_ts = metric_ts.astimezone().replace(tzinfo=None)
            except (TypeError, ValueError):
                logger.warning("设备上报了无效时间戳，改用服务端时间: %s",
                               device_id)
                metric_ts = datetime.now()
            received_at = datetime.now()

            data = payload.get("data", {})
            if not isinstance(data, dict):
                logger.warning("设备上报 data 不是对象: %s", device_id)
                await session.commit()
                raise BadMessageError("data not an object")

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
                    logger.warning("忽略非数值指标 %s: %s", metric, device_id)
                    continue
                try:
                    numeric = float(value)
                except OverflowError:
                    logger.warning("忽略超出范围的指标 %s: %s", metric, device_id)
                    continue

                metric_records.append(
                    Metric(
                        device_id=device_id,
                        metric=metric,
                        value=numeric,
                        ts=metric_ts,
                        received_at=received_at,
                    )
                )
                saved_metrics.append(metric)

            if metric_records:
                session.add_all(metric_records)
                await session.flush()

            # on_off 确认在途开关命令；状态归属按 received_at 判断。
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

    async def _handle_status(
        self,
        gateway_id: str,
        node_id: str,
        payload: dict[str, Any],
        retained: bool = False,
    ) -> None:
        """处理设备状态。

        - retained 是 broker 保存的最后状态，可能很旧：不据此创建设备、
          不刷新 last_seen，避免幻影在线；
        - 非 retained 由设备切实发出，才更新心跳；
        - 非法状态值归一化为 unknown 并正常落库。
        """
        device_id = f"{gateway_id}-{node_id}"

        async with async_session_factory() as session:
            device = await session.get(Device, device_id)

            if device is None:
                if retained:
                    logger.info("忽略无对应设备的 retained 状态: %s", device_id)
                    return
                await self._ensure_device(
                    session, device_id, node_id, "sensor", gateway_id
                )
                device = await session.get(Device, device_id)

            raw_status = payload.get("status")
            status = raw_status if raw_status in ("online", "offline") else "unknown"

            device.status = status
            if not retained:
                device.last_seen = datetime.now()
            await session.commit()
            suffix = "（retained，last_seen 未更新）" if retained else ""
            logger.info("设备状态更新: %s -> %s%s", device_id, status, suffix)

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
            await self.client.publish(
                topic, json.dumps(message).encode(), qos=1
            )
            logger.info("命令已发送: %s -> %s", device_id, command)
            return True

        except Exception:
            logger.exception("发送命令失败: %s", device_id)
            return False


# 全局 MQTT 服务实例
mqtt_service = MQTTService()
