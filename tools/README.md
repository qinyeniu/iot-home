# tools — 测试、烧录与诊断工具

本目录用于保存开发过程中的串口采集、日志分析、MQTT 联调、部署辅助和一次性迁移脚本。运行任何会写设备的工具前，必须先确认端口和目标固件，避免把实验固件烧入现网设备。

## 当前常用工具

| 工具 | 作用 |
| --- | --- |
| `capture_serial.py` | 按指定时长读取串口日志 |
| `capture_serial_stream.py` | 串口流式记录；使用前需确认是否会触发复位 |
| `capture_serial_no_reset.py` | 尽量保持 DTR/RTS inactive 的被动串口记录 |
| `analyze_serial_log.py` | 流式分析单份 ESP 串口日志，识别重启、panic、MAC no-buffer、Zigbee/MQTT/Wi‑Fi 等 |
| `analyze_long_test.py` | 同时分析节点/网关双日志，识别网关 RF 下发持续失败，输出 `PASS / DEGRADED / FAIL / INCOMPLETE` 报告 |
| `analyze_overnight_logs.py` | 旧版整夜双日志分析脚本，保留作历史兼容 |
| `rf_power_config.py` | 通过 MQTT 执行 RF QUERY/SET/AUTO 联调 |
| `simulator.py` | 模拟设备向 MQTT 发布数据 |
| `deploy_server.py` / `interactive_deploy.ps1` | 服务器部署辅助；未经用户明确要求不执行 |

## 长测分析器用法

```powershell
python tools/analyze_long_test.py `
  firmware/node_zigbee/log.long-node-*.txt `
  firmware/gateway/log.long-gateway-*.txt `
  --target-duration 28800 `
  --rf-failure-run-threshold 5
```

判定规则：

- `FAIL`：assert/panic/abort、重启标志、uptime 回跳、MAC no-buffer 风暴，或网关连续 RF 下发失败达到阈值（默认 5 次）；
- `DEGRADED`：节点 APS/I2C/队列失败、观察缺口、未达风暴阈值的 MAC no-buffer、单次 RF 明确失败或 RF 结果无法解析；
- `INCOMPLETE`：目标时长未满足，且未发现失败或降级线索；
- `PASS`：达到目标时长且未发现异常。

## 本机环境（2026-09-25）

| 项目 | 状态 |
| --- | --- |
| Python | 3.14.0 |
| ESP-IDF | v5.5.4，安装于 `C:\Espressif\v5.5.4\esp-idf` |
| Zigbee 组件 | 现网/电池工程当前按 1.6.0 固定 |
| pytest | 系统 Python 未全局安装；仓库 `.venv` 中已安装，`.venv` 不提交 |
| Docker | 仅本地后端回归时按需启动 |

运行新工具的定向测试：

```powershell
.\.venv\Scripts\python.exe -m pytest tools/tests/test_analyze_long_test.py -q
```

## 安全边界

- 不读取、不打印、不复制 `wifi_secrets.h`、`mqtt_secrets.h` 等密码文件；
- 不使用 `git add -A`；
- 不刷 bootloader/分区表、不整片擦 Flash、不擦 Zigbee NVRAM，除非用户明确授权；
- 不自动部署或重启远程服务器；
- 打开串口可能触发板子复位，开始长测前必须让用户知道。

## 临时脚本

目录中仍有较多历史补丁和迁移脚本。它们是前期开发过程的证据，后续应按 `docs/WORKSPACE_CLEANUP_INVENTORY.md` 分类后再归档或删除；当前不做批量清理。
