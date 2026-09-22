# MQTT 认证与 ACL 加固说明

## 目标

当前公网 MQTT 曾被观察到疑似匿名可连，风险是任何知道地址的人都可能订阅设备数据或发布控制命令。本次加固在不立即部署的前提下，先把最小权限配置、迁移步骤、回退方案和公网 TLS 要求准备好。

## 主题与账号矩阵

当前主题约定：

```text
iot-home/{gateway_id}/nodes/{node_id}/telemetry
iot-home/{gateway_id}/nodes/{node_id}/status
iot-home/{gateway_id}/nodes/{node_id}/rf_policy
iot-home/{gateway_id}/nodes/{node_id}/cmd
```

| 账号 | telemetry/status/rf_policy | cmd | 限制范围 |
| --- | --- | --- | --- |
| 后端账号 | 只读 | 只写 | 可访问所有网关 |
| 网关账号 | 只写 | 只读 | 仅限 `MQTT_GATEWAY_ID` 指定网关 |
| 迁移旧账号 | 读写整个前缀 | 读写整个前缀 | 仅短期共存，默认关闭 |
| 匿名用户 | 拒绝 | 拒绝 | 无 |

当前固件网关 ID 为 `gw-001`。

## 配置文件

- `server/mosquitto/config/mosquitto.conf`
  - 1883：原生 MQTT；
  - 9001：WebSocket；
  - 认证和 ACL 对两个监听器统一生效。
- `server/mosquitto/config/generate_password.sh`
  - 从环境变量生成 Mosquitto 密码文件和 ACL；
  - 密码通过标准输入交给 `mosquitto_passwd`，避免出现在进程参数中；
  - 用户名、网关 ID、主题前缀使用白名单校验，防止换行符或通配符注入额外 ACL；
  - 新分离账号密码至少 12 位，并拒绝明显占位值。
- `server/.env.example`
  - 提供后端/网关分离账号和迁移开关的环境变量模板，不含真实密码。

## 三种账号模式

### 1. 最终最小权限模式

配置：

```env
MQTT_BACKEND_USER=...
MQTT_BACKEND_PASSWORD=...
MQTT_GATEWAY_USER=...
MQTT_GATEWAY_PASSWORD=...
MQTT_GATEWAY_ID=gw-001
MQTT_ENABLE_LEGACY_ACCOUNT=false
```

Broker 只创建后端和网关两个低权限账号。

### 2. 迁移共存模式

配置：

```env
MQTT_ENABLE_LEGACY_ACCOUNT=true
MQTT_USER=...
MQTT_PASSWORD=...
```

Broker 同时创建后端、网关和旧共享账号。这样可先更新 Broker，再逐个烧录网关，避免设备在切换窗口离线。

### 3. 旧账号回退模式

清空后端/网关四个分离账号变量，仅保留：

```env
MQTT_USER=...
MQTT_PASSWORD=...
```

该模式拥有整个 `MQTT_TOPIC_PREFIX/#` 的读写权限，只适合开发或紧急回退；仍然保持 `MQTT_ALLOW_ANONYMOUS=false`。

## 推荐迁移步骤

1. 在服务器 `.env` 中加入：
   - `MQTT_BACKEND_USER` / `MQTT_BACKEND_PASSWORD`；
   - `MQTT_GATEWAY_USER` / `MQTT_GATEWAY_PASSWORD`；
   - `MQTT_GATEWAY_ID=gw-001`；
   - `MQTT_ENABLE_LEGACY_ACCOUNT=true`。
2. 重建 Mosquitto。此时新旧账号都能认证，匿名仍保持关闭。
3. 更新网关固件使用网关账号，烧录并确认能连接 Broker、发布上报、接收命令。
4. 确认后端使用独立账号能收到 telemetry/status/rf_policy 并发布 cmd。
5. 设置 `MQTT_ENABLE_LEGACY_ACCOUNT=false`，再次重建 Mosquitto，移除旧账号。
6. 验证：
   - 匿名连接被拒绝；
   - 后端不能发布 telemetry/status/rf_policy；
   - 后端不能订阅 cmd；
   - 网关不能订阅其他网关的 cmd；
   - 网关不能伪装成其他网关发布上报；
   - 网关不能向自己的 cmd 发布命令。
7. 观察稳定后，从最终 `.env` 中删除旧 `MQTT_USER` / `MQTT_PASSWORD`。

## 回退方案

如果新账号导致设备无法连接，可在服务器 `.env` 中暂时清空四个分离账号变量，恢复旧 `MQTT_USER` / `MQTT_PASSWORD`。此时后端和网关共用一个过渡账号，但仍应保持 `MQTT_ALLOW_ANONYMOUS=false`。

## 公网生产阻断项：明文 MQTT/WebSocket

当前 1883/9001 是明文协议。只要端口直接暴露公网，就存在以下风险：

- MQTT 用户名和密码可被窃听；
- 攻击者可冒充网关或后端；
- 遥测和命令可能被读取或篡改；
- 密码认证可能被在线爆破。

因此公网生产前必须至少选择一种保护方式：

1. MQTT over TLS（推荐 8883）与 WebSocket over TLS（推荐 8084/WSS）；
2. 云安全组或主机防火墙仅允许网关出口 IP；
3. VPN / WireGuard / Tailscale 内网访问。

若使用 TLS，网关固件必须验证 Broker CA，不能只启用加密而不验证服务器。多网关长期方案建议使用每设备独立客户端证书。若此前明文 MQTT 已长期运行，完成 TLS 迁移后应轮换后端和网关密码。

## 仍需后续处理

1. 准备 TLS 证书、Mosquitto TLS listener 与后端/固件 TLS 连接配置；
2. 网关 MQTT 凭据仍是编译时配置，后续可做入网配置页或本地配网；
3. 多网关部署前，应为每个网关创建独立账号或使用证书身份；
4. 增加连接速率限制或防爆破策略。
