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

## 资源保护覆盖文件（默认不生效）

`docker-compose-resource-guard.yml` 提供小规格阿里云 ECS 的资源上限和容器日志轮转，默认不会被 `docker compose up -d` 使用。

后续维护窗口先进入 `server/` 目录，再预览合并结果：

```bash
cd server
docker compose -f docker-compose.yml \
  -f docker-compose-resource-guard.yml config
```

确认无误后，再显式使用该覆盖文件启动：

```bash
docker compose -f docker-compose.yml \
  -f docker-compose-resource-guard.yml up -d
```

命令只作用于当前 Docker host（本机、`DOCKER_HOST` 或当前 Docker context 指向的主机）；如果在阿里云 ECS 的 SSH 会话中执行，就属于一次手动部署/容器重建。执行后检查容器状态、内存、磁盘和业务链路；资源预算说明见 `docs/ALIYUN_SERVER_MAINTENANCE_PLAN.md`。未经用户明确要求，不自动部署到阿里云。
