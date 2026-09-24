# 阿里云 ECS：MQTT 账号 / ACL 部署与回退手册

更新时间：2026-09-24  
适用服务器：**阿里云 ECS**  
适用范围：Mosquitto 账号分离、关闭匿名、最小权限 ACL  
状态：**仅手册，未远程执行；需用户明确选择维护窗口后共同操作**

## 0. 本手册不做什么

本阶段只处理 MQTT 认证和授权：

- 后端与网关使用不同 MQTT 账号；
- 迁移窗口短期保留旧账号；
- 保持匿名访问关闭；
- 用 ACL 限制跨网关读写。

本阶段不处理：

- TLS/WSS；
- VPN；
- 阿里云安全组；
- 域名和证书；
- Docker 镜像升级；
- MySQL/Grafana 配置变更。

因此不修改网卡、不切换热点、不改变服务器公网端口，也不打开匿名访问。后续 TLS/WSS 或 VPN 必须使用单独维护窗口。

## 1. 推荐路线

```text
窗口 1：备份到仓库外 → 新旧账号共存 → 重建 Mosquitto 和后端
  ↓
验证后端独立账号、网关账号边界和业务链路
  ↓
设备端切换到网关账号，观察至少一个完整上报和命令周期
  ↓
窗口 2：移除旧账号 → 验证匿名拒绝、越权拒绝、遥测和命令正常
  ↓
观察 1–3 天，再进入 TLS/WSS 或 VPN 阶段
```

如果当前网关固件仍使用旧共享账号，第一次窗口结束后不能移除旧账号，否则网关会因认证失败离线。

## 2. 开始前确认

下文中的占位路径需要按服务器实际情况替换：

- `<SERVER_PROJECT_DIR>/server`：服务器上的 Compose 项目目录；
- `<BACKUP_PARENT>`：仓库外的私有备份目录，例如 `/root/backups` 或 `/home/<user>/private-backups`。

开始前确认：

1. 当前不是游戏、工作或网络不能中断的时间段；
2. 可以接受 Mosquitto 和后端短暂中断；
3. 能通过 SSH 或阿里云控制台连接 ECS；
4. `<BACKUP_PARENT>` 位于 Git 仓库外；
5. 已确认当前真实网关主题和网关 ID；
6. 旧账号密码、新后端密码、新网关密码都符合当前脚本的基础强密码规则。

当前脚本会拒绝：

- 空密码或包含控制字符的密码；
- 少于 12 个字符的密码；
- 包含 `changeme`、`password`、`改成` 等明显占位特征的密码。

脚本只做基础检查，执行人仍必须确认密码不是文档中的提示文字。迁移期旧账号密码也会被检查；如果旧密码不足 12 位或不符合规则，需要先安排单独的密码轮换窗口，不能等 Mosquitto 启动失败后再临时处理。

## 3. 窗口 1：建立新旧账号共存

### 3.1 进入项目并检查状态

```bash
cd <SERVER_PROJECT_DIR>/server
docker compose ps
docker stats --no-stream
df -h /
free -m
```

建议维护前满足：

- 系统盘至少保留 25% 或 3–5 GB 空闲；
- 内存至少保留约 20% 空闲；
- 没有异常高 CPU 容器；
- Mosquitto、后端、MySQL、Grafana 容器状态可识别。

### 3.2 确认匿名开关和实际网关 ID

只检查键名和值，不打印密码：

```bash
grep -E '^MQTT_ALLOW_ANONYMOUS=' .env
grep -E '^MQTT_GATEWAY_ID=' .env
grep -E '^MQTT_TOPIC_PREFIX=' .env
```

目标值应为：

```env
MQTT_ALLOW_ANONYMOUS=false
MQTT_GATEWAY_ID=gw-001
MQTT_TOPIC_PREFIX=iot-home
```

如果维护前已经是 `MQTT_ALLOW_ANONYMOUS=true`，先记录这个事实；本窗口应改为 `false`，回退时也不应盲目恢复匿名。

`MQTT_GATEWAY_ID` 必须和真实主题一致。可通过现有 Mosquitto/后端日志、最近数据库记录或最近消息确认。若真实主题不是 `iot-home/gw-001/...`，必须按实际网关 ID 填写，否则网关账号会因 ACL 路径不匹配而无法发布。

### 3.3 将配置备份到 Git 仓库外

```bash
backup_stamp="$(date +%Y%m%d-%H%M%S)"
backup_dir="<BACKUP_PARENT>/iot-home/mqtt-acl-$backup_stamp"

install -d -m 700 "$backup_dir"
cp .env "$backup_dir/.env"
cp docker-compose.yml "$backup_dir/docker-compose.yml"
cp mosquitto/config/mosquitto.conf "$backup_dir/mosquitto.conf"
chmod 600 "$backup_dir/.env"
```

确认备份目录不在当前 Git 仓库内：

```bash
git -C <SERVER_PROJECT_DIR> rev-parse --show-toplevel
realpath "$backup_dir"
```

不要把 `$backup_dir/.env` 复制回仓库、提交到 Git、打印到终端或截图展示。

### 3.4 修改服务器 `.env`

在服务器本地编辑 `.env`，确认以下配置。密码在本地输入，不写入本文档：

```env
MQTT_TOPIC_PREFIX=iot-home
MQTT_ALLOW_ANONYMOUS=false

MQTT_BACKEND_USER=iot_backend
MQTT_BACKEND_PASSWORD=填写后端独立强密码

MQTT_GATEWAY_USER=iot_gateway_gw001
MQTT_GATEWAY_PASSWORD=填写网关独立强密码
MQTT_GATEWAY_ID=gw-001

MQTT_ENABLE_LEGACY_ACCOUNT=true
MQTT_USER=旧共享账号用户名
MQTT_PASSWORD=旧共享账号密码
```

用户名只允许字母、数字、下划线、点或连字符；后端用户名、网关用户名和旧账号用户名必须不同。

如果密码包含特殊 shell 字符，可在 `.env` 中使用单引号包裹，例如：

```env
MQTT_BACKEND_PASSWORD='一段强密码'
```

不要把单引号误认为密码的一部分，除非密码本身确实包含单引号。

### 3.5 按最小范围重建 Mosquitto 和后端

先重建 Mosquitto：

```bash
docker compose up -d --no-deps --no-build --force-recreate mosquitto
```

后端容器不会因为只重建 Mosquitto 而重新读取 `.env`，所以还必须重建后端：

```bash
docker compose up -d --no-deps --no-build --force-recreate backend
```

参数含义：

- `--no-deps`：不联动 MySQL、Grafana 等依赖服务；
- `--no-build`：不在服务器现场意外构建镜像；
- 显式指定服务名：不重建 MySQL 或 Grafana。

后端会有短暂 API 中断，属于本维护窗口预期。

### 3.6 查看启动结果

```bash
docker compose ps mosquitto backend
docker compose logs --since=5m mosquitto
docker compose logs --since=5m backend
```

Mosquitto 正常日志应包含：

```text
Mosquitto password and ACL files generated
```

如果 Mosquitto 或后端反复退出，停止继续操作，按第 7 节回退或排查。

### 3.7 验证匿名访问被拒绝

```bash
docker exec iot-mosquitto \
  mosquitto_sub -h 127.0.0.1 -p 1883 \
  -t '$SYS/#' -C 1 -W 3
```

预期结果：连接被拒绝，通常会出现：

```text
Connection error: Connection Refused: not authorised.
```

如果无认证也能连接或读取，立即停止，不要继续后续步骤。

### 3.8 验证后端账号订阅授权

使用 shell 变量读取密码，避免密码进入命令历史：

```bash
read -rs -p 'Backend MQTT password: ' mqtt_pw
printf '\n'

docker exec -i iot-mosquitto \
  mosquitto_sub -h 127.0.0.1 -p 1883 \
  -u iot_backend -P "$mqtt_pw" \
  -t 'iot-home/+/nodes/+/telemetry' \
  -W 10

unset mqtt_pw
```

如果 10 秒内没有新遥测，命令会因等待超时而结束；只要没有提示 not authorised，即可认为订阅授权通过。若节点正好上报，应看到遥测 JSON。

这种方式仍可能让密码短时间出现在容器进程参数中；执行时避免截图和不必要的录屏。

### 3.9 验证后端账号不能向未授权主题发布

```bash
read -rs -p 'Backend MQTT password: ' mqtt_pw
printf '\n'

docker exec -i iot-mosquitto \
  mosquitto_pub -h 127.0.0.1 -p 1883 \
  -u iot_backend -P "$mqtt_pw" \
  -t 'iot-home/acl-probe' \
  -m 'probe'

unset mqtt_pw
```

预期结果：发布被 ACL 拒绝。该主题不是 `cmd`、`telemetry` 或 `status`，即使配置异常也更不容易被识别成真实业务消息。

### 3.10 验证网关账号只能访问自己的网关

验证网关账号可以订阅自己的命令主题：

```bash
read -rs -p 'Gateway MQTT password: ' mqtt_pw
printf '\n'

docker exec -i iot-mosquitto \
  mosquitto_sub -h 127.0.0.1 -p 1883 \
  -u iot_gateway_gw001 -P "$mqtt_pw" \
  -t 'iot-home/gw-001/nodes/+/cmd' \
  -W 5

unset mqtt_pw
```

无测试命令时会等待超时；只要没有提示 not authorised，即可认为订阅授权通过。

验证网关账号不能订阅其他网关：

```bash
read -rs -p 'Gateway MQTT password: ' mqtt_pw
printf '\n'

docker exec -i iot-mosquitto \
  mosquitto_sub -h 127.0.0.1 -p 1883 \
  -u iot_gateway_gw001 -P "$mqtt_pw" \
  -t 'iot-home/gw-999/nodes/+/cmd' \
  -W 3

unset mqtt_pw
```

预期结果：订阅被拒绝。该命令只读取，不发布消息。

验证网关账号不能向其他网关发布：

```bash
read -rs -p 'Gateway MQTT password: ' mqtt_pw
printf '\n'

docker exec -i iot-mosquitto \
  mosquitto_pub -h 127.0.0.1 -p 1883 \
  -u iot_gateway_gw001 -P "$mqtt_pw" \
  -t 'iot-home/gw-999/acl-probe' \
  -m 'probe'

unset mqtt_pw
```

预期结果：发布被拒绝。这里使用非业务 `acl-probe` 主题，避免向具有 telemetry 结构的主题写入测试内容。

### 3.11 验证业务链路

```bash
curl -fsS http://127.0.0.1:8000/api/health
docker compose logs --since=10m backend
docker compose logs --since=10m mosquitto
```

预期健康结果包含：

```json
"status": "healthy",
"mqtt_connected": true
```

确认：

- 后端使用独立 MQTT 账号重连成功；
- 节点遥测继续入库；
- API 可访问；
- Grafana 可继续查询；
- 没有持续出现账号认证失败或 ACL denied。

如果当前网关仍使用旧账号，本阶段必须保留：

```env
MQTT_ENABLE_LEGACY_ACCOUNT=true
```

## 4. 设备端切换到网关账号

只有在本地配网 / NVS 凭据功能准备好，并明确如何让网关保存新 MQTT 用户名和密码后，才执行设备切换。

切换前确认：

1. 网关账号和密码输入正确；
2. Broker 的网关账号 ACL 已验证；
3. 旧账号仍处于短期共存状态；
4. 切换失败时，网关还能改回旧账号或继续使用旧配置。

切换成功后至少观察：

- 网关上线状态；
- 一轮到多轮遥测；
- 一次远程命令；
- 网关重连或节点拔插后的恢复情况。

在网关确认使用独立账号前，不移除旧账号。

## 5. 窗口 2：移除旧账号

### 5.1 修改 `.env`

确认网关已经使用独立网关账号后，将：

```env
MQTT_ENABLE_LEGACY_ACCOUNT=false
```

保持：

```env
MQTT_ALLOW_ANONYMOUS=false
```

不要通过清空 `MQTT_USER` / `MQTT_PASSWORD` 替代开关；脚本以 `MQTT_ENABLE_LEGACY_ACCOUNT=false` 作为正式移除旧账号的依据。

### 5.2 重建 Mosquitto

后端已经在窗口 1 加载了独立账号，因此本步通常只需要重建 Mosquitto：

```bash
docker compose up -d --no-deps --no-build --force-recreate mosquitto
docker compose logs --since=5m mosquitto
```

### 5.3 回归验证

重复以下验证：

1. 匿名连接被拒绝；
2. 后端账号可以订阅遥测；
3. 后端账号不能向未授权主题发布；
4. 网关账号可以订阅自己的命令主题；
5. 网关账号不能订阅或访问其他网关；
6. `/api/health` 返回 MQTT 已连接；
7. 最新遥测继续入库；
8. Grafana 数据继续更新；
9. 远程命令仍可发送并收到确认。

建议观察 1–3 天，再进入 TLS/WSS 或 VPN 阶段。

## 6. 验收标准

本阶段完成的标准是：

- `MQTT_ALLOW_ANONYMOUS=false`；
- `MQTT_ENABLE_LEGACY_ACCOUNT=false`；
- 后端使用独立后端账号；
- 网关使用独立网关账号；
- 网关账号不能跨网关订阅或发布；
- 后端不能向非命令主题发布；
- 遥测、状态、RF policy 和命令链路正常；
- Mosquitto 日志中没有持续认证或授权错误；
- 服务器 CPU、内存和磁盘没有明显异常增长。

## 7. 回退方案

### 7.1 新旧账号共存阶段失败

如果新账号或 ACL 导致后端无法正常工作，可从仓库外备份恢复 `.env`：

```bash
cd <SERVER_PROJECT_DIR>/server
backup_dir="<BACKUP_PARENT>/iot-home/替换为实际备份目录"
cp "$backup_dir/.env" .env
```

恢复后不要立即启动容器，先人工检查并修正：

```bash
grep -E '^MQTT_ALLOW_ANONYMOUS=' .env
```

如果该值不是 `false`，先编辑为：

```env
MQTT_ALLOW_ANONYMOUS=false
```

然后按最小范围重建：

```bash
docker compose up -d --no-deps --no-build --force-recreate mosquitto
docker compose up -d --no-deps --no-build --force-recreate backend
```

该回退可能恢复旧共享账号，但不应通过打开匿名访问回退。

### 7.2 移除旧账号后设备端无法连接

如果网关切换失败，而旧账号已经被移除，可以临时恢复新旧账号共存：

```env
MQTT_ENABLE_LEGACY_ACCOUNT=true
MQTT_ALLOW_ANONYMOUS=false
```

保留独立后端和网关账号，同时配置旧 `MQTT_USER` / `MQTT_PASSWORD`，然后：

```bash
docker compose up -d --no-deps --no-build --force-recreate mosquitto
```

这仍然要求所有连接提供账号密码，不通过匿名访问回退。

### 7.3 Mosquitto 无法启动

先查看具体错误：

```bash
docker compose logs --since=10m mosquitto
```

常见原因包括：

- 密码长度或用户名不符合脚本规则；
- 新账号用户名相同；
- `MQTT_GATEWAY_ID` 与设备实际主题不一致；
- `.env` 引号或换行错误；
- 卷内文件权限异常。

若短时间无法修复，按第 7.1 恢复配置备份。不要通过删除 `password_file` 或设置 `allow_anonymous true` 强制启动。

## 8. 资源保护和迁移期残留风险

维护操作本身应遵守：

- 只重建 Mosquitto 和后端，不执行全量镜像构建；
- 不同时运行备份、系统升级、证书申请或容器重建；
- 不把 Mosquitto 日志调得过于冗长；
- 每次维护后检查一次磁盘、内存和容器状态。

当前账号 / ACL 窗口仍有迁移期残留风险：

- 1883/9001 在 TLS/VPN 完成前仍是明文协议；
- `max_connections -1` 不能阻止公网大量 TCP 连接尝试；
- 认证能阻止未授权消息，但不能完全消除连接资源消耗。

后续通过 TLS、安全组白名单、VPN 和按设备规模设置合理连接上限解决。不要在当前账号窗口盲目设置过低连接上限，以免误杀真实设备。

## 9. 后续教学顺序

账号 / ACL 稳定后，再按以下顺序教学：

1. 域名和 DNS；
2. CA / 证书生命周期；
3. Mosquitto TLS 8883；
4. 浏览器 WSS；
5. ESP32-C6 内置 CA 根证书；
6. 关闭明文 1883/9001；
7. VPN 或安全组白名单。

这些内容不在本手册中执行。
