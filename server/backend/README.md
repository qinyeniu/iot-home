# backend — FastAPI 后端

规划（第 1 周代码阶段实现）：

- MQTT 客户端订阅 `iot-home/+/nodes/+/telemetry`、`/status` 等主题；
- 指标表写入：`device_id + metric + ts + value`，任何新指标自动兼容；
- 设备注册与命令转发 API（第 5–7 周随远程控制接口一起扩展）；
- 配置全部来自环境变量（`server/.env`），本地与阿里云共用同一套代码。
