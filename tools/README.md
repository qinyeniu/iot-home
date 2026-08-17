# tools — 模拟器与调试工具

## 用途

- 模拟传感器节点周期性发布温湿度 / 光照数据到 MQTT；
- 硬件到货前即可完成"模拟设备 → MQTT → 入库 → Grafana 曲线"端到端联调。
- 串口联调：通过 COM 口读取设备日志与测试数据（pyserial 脚本），辅助硬件验收与固件调试；
- 烧录与验证：esptool 读取芯片 ID、烧录固件、查看 flash 信息。

## 本机工具链现状（2026-08-17 实测）

| 工具 | 状态 | 说明 |
|------|------|------|
| Python | ✅ 3.14.0 | `C:\Users\HJB\AppData\Local\Python\bin\python.exe`，PATH 已配置 |
| Node.js | ✅ v22.19.0 | — |
| Docker | ⚠️ 已装 29.6.1 | 守护进程未启动，使用前需启动 Docker Desktop |
| 串口 | ✅ COM1 / COM2 | 设备插入 USB 后会出现新的 COM 口 |
| esptool / pyserial | 待安装 | 用 `python -m pip install esptool pyserial` 安装 |
| ESP-IDF | 未安装 | 第 3 周固件开发前安装（约 5GB，需预留磁盘） |
