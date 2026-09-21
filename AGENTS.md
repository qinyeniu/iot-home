# AGENTS.md — IoT-Home 长期工作规则（每次会话必须先读）

本项目为 ESP32-C6 三层架构 IoT 原型（Zigbee 节点 → 网关 → Wi‑Fi/MQTT → FastAPI → MySQL），全程中文、设计先行、硬件实测驱动。

## 铁律：每次必须写交接文档（不可省略）

**每一次会话结束前、每完成一个可交付阶段后，都必须更新本地交接文档，把当前进度、决策、实测结果、待办写入文件。**

原因：agent 的上下文经过压缩后总会丢失部分记忆；模型记忆不可靠，**只有写入仓库内的本地文档才能稳定保留上下文**。因此无论本次改动多小，都必须落盘，不能只停留在对话里。

执行方式：

1. 当天完整交接：`docs/HANDOFF_YYYY-MM-DD.md`；
2. 当天入口摘要：`docs/HANDOFF/_YYYY-MM-DD.md`（新对话先读）；
3. 跨天继续时：先读最新入口 → 再读完整交接 → 然后才能动手；
4. 目录规则与历史索引见 `docs/HANDOFF/README.md`。

## 硬性边界

- 不 push、不部署服务器（除非用户明确要求）；
- 不整片擦 Flash、不擦 Zigbee NVRAM、不刷 bootloader/分区表（除非用户明确要求）；
- 不要 `git add -A`，只显式添加本次相关文件；
- 不读取、不打印、不落文档：wifi_secrets.h / mqtt_secrets.h 等密码文件；
- 打开 COM5/COM6 串口监视器本身可能触发板子复位。

## 开始工作前

1. 读 `docs/HANDOFF/README.md` 找到最新入口；
2. 读最新 `docs/HANDOFF/_YYYY-MM-DD.md` 与对应完整交接；
3. 工作结束后，按铁律更新交接文档并提交。
