# mysql/init — 数据库初始化脚本

容器首次启动时自动执行：

- 创建独立数据库 `iot_home`（utf8mb4）；
- 创建设备表与**指标表**（`device_id + metric + ts + value`），新指标免改表。
