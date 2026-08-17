# server — 服务端

**本地测试优先**：Windows + Docker Compose 一键起整套环境
（Mosquitto + MySQL + FastAPI + Grafana），同一套代码通过环境变量切换部署到阿里云。

```
server/
├── backend/             # FastAPI 后端（MQTT 订阅入库 + 设备/命令 API）
├── docker-compose.yml   # 本地一键环境（第 1 周代码阶段提供）
├── grafana/             # 数据源与看板 provisioning
├── mysql/init/          # 建库建表初始化脚本
├── mosquitto/config/    # MQTT 配置
└── .env.example         # 环境变量模板（复制为 .env 后填写）
```

> 阿里云 ECS 部署（Mosquitto 安装、安全组、systemd）已**后置**，
> 等服务器资源优化完成后另行安排，不影响本地开发进度。
