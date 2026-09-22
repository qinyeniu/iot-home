"""射频发射功率策略的运行时状态跟踪。

网关通过 MQTT 主题 ``.../rf_policy`` 发布节点当前的发射功率与模式。
本模块：

- 缓存每个设备最近一次策略状态；
- 按一次性 ``request_id`` 挂起等待者，只有与本次操作匹配的
  ``status="confirmed"`` 报文才会完成等待；
- ``querying`` / ``report_received`` 等中间状态不能确认用户操作。

该状态只保存在后端进程内存中；长期历史由 Metric 表保存。
"""

from __future__ import annotations

import asyncio
import secrets
from dataclasses import dataclass
from datetime import datetime
from typing import Any, Optional

SUPPORTED_POWERS = (-10, 0, 8, 14, 18, 20)

# 请求类型，与 tools/rf_power_config.py 的确认语义保持一致。
KIND_QUERY = "query"
KIND_MANUAL = "manual"
KIND_AUTO = "auto"

VALID_KINDS = (KIND_QUERY, KIND_MANUAL, KIND_AUTO)


def new_request_id() -> str:
    """生成 16 字符一次性事务编号。"""
    return secrets.token_hex(8)


@dataclass
class _Pending:
    device_id: str
    kind: str
    target_power: Optional[int]
    future: asyncio.Future
    created_ms: int

    def matches_confirmed(self, state: dict[str, Any]) -> bool:
        power = state.get("power_dbm")
        mode = state.get("mode")

        if not isinstance(power, int) or isinstance(power, bool):
            return False
        if power not in SUPPORTED_POWERS:
            return False

        if self.kind == KIND_QUERY:
            return mode in ("auto", "manual")
        if self.kind == KIND_AUTO:
            return mode == "auto"
        # manual：必须锁定手动模式且功率精确等于目标档位。
        return mode == "manual" and power == self.target_power


class RFPolicyStates:
    """内存中的设备策略状态与等待者表。"""

    def __init__(self) -> None:
        self._states: dict[str, dict[str, Any]] = {}
        self._pending: dict[str, _Pending] = {}

    @staticmethod
    def _now_ms() -> int:
        loop = asyncio.get_running_loop()
        return int(loop.time() * 1000)

    def get_state(self, device_id: str) -> Optional[dict[str, Any]]:
        return self._states.get(device_id)

    def register_waiter(
        self,
        request_id: str,
        device_id: str,
        kind: str,
        target_power: Optional[int],
    ) -> asyncio.Future:
        """登记一个等待 confirmed 报文的 Future。"""
        loop = asyncio.get_running_loop()
        pending = _Pending(
            device_id=device_id,
            kind=kind,
            target_power=target_power,
            future=loop.create_future(),
            created_ms=self._now_ms(),
        )
        self._pending[request_id] = pending
        return pending.future

    def drop_waiter(self, request_id: str) -> None:
        self._pending.pop(request_id, None)

    def update_state(
        self,
        device_id: str,
        state: dict[str, Any],
        *,
        retained: bool = False,
    ) -> Optional[_Pending]:
        """用网关上报更新缓存；若完成了某个等待事务则返回该等待项。"""
        stored = dict(state)
        stored["device_id"] = device_id
        stored["received_at"] = datetime.now().isoformat(timespec="seconds")
        stored["retained"] = bool(retained)
        self._states[device_id] = stored

        request_id = stored.get("request_id")
        if (
            stored.get("status") != "confirmed"
            or not isinstance(request_id, str)
        ):
            return None

        pending = self._pending.get(request_id)
        if (
            pending is None
            or pending.device_id != device_id
            or pending.future.done()
            or not pending.matches_confirmed(stored)
        ):
            return None

        pending.future.set_result(stored)
        return pending

    def snapshot(self) -> dict[str, dict[str, Any]]:
        """返回全部缓存（诊断用）。"""
        return {k: dict(v) for k, v in self._states.items()}

    def reset(self) -> None:
        """测试或后端关停时清空状态。"""
        for pending in self._pending.values():
            if not pending.future.done():
                pending.future.cancel()
        self._states.clear()
        self._pending.clear()


# 全局单例
rf_policy_states = RFPolicyStates()
