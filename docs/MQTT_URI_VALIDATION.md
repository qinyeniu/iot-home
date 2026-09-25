# MQTT Broker URI 格式校验

更新时间：2026-09-25  
状态：阶段 1b 已实现并通过主机测试；尚未接入网关启动、NVS 保存或 SoftAP 接口  
适用对象：ESP32-C6 网关

## 1. 目标

在本地配网页面接收 MQTT Broker 地址后，先确认 URI 的结构可被 ESP-MQTT 识别，避免用户把错误地址保存进设备。该校验只检查格式，不发起网络连接，也不会实际启用 TLS、WSS 或修改云服务器。

## 2. 支持范围

| Scheme | 默认能力 | 当前是否启用安全连接 | 路径 |
|---|---|---|---|
| `mqtt://` | 明文 MQTT over TCP | 当前实验路径 | 不允许路径 |
| `mqtts://` | MQTT over TLS | 仅语法预留，未配置 | 不允许路径 |
| `ws://` | 明文 MQTT over WebSocket | 仅语法预留，未配置 | 必须有路径 |
| `wss://` | MQTT over secure WebSocket | 仅语法预留，未配置 | 必须有路径 |

协议头大小写不敏感，`MQTT://`、`Wss://` 会按对应协议处理。允许：

- DNS 主机，例如 `broker.example.com`；
- IPv4 地址，例如 `8.163.110.27`；
- 可选端口，例如 `:1883`；
- WebSocket 路径和查询参数，例如 `/mqtt?protocol=3.1.1`；
- WebSocket 路径中使用 percent-encoding，例如 `%41`，但拒绝原始换行、制表符等 C0/DEL 控制字符。

暂不支持：

- IPv6 literal；
- URI 内嵌入 `username:password@host`；
- fragment（`#...`）；
- 截断或非法 percent-encoding；
- 百分号编码主机名。

用户名和密码必须通过产品自身的独立字段提交，避免重复来源和凭据泄露。

## 3. 主机规则

IPv4 必须是四个 0–255 的十进制段，并拒绝会被部分系统按八进制理解的前导零。

DNS 名称要求：

- 总长度不超过 253；
- 标签非空且不超过 63；
- 允许字母、数字、连字符、下划线、点和波浪号；
- 不允许空标签、开头/结尾的点。

下划线不是最严格的公网主机名字符，但部分家庭/局域网名称可能使用，因此当前保留兼容性。后续若要严格按公网 DNS 校验，可以单独收紧。

## 4. API

```c
iot_mqtt_uri_error_t iot_mqtt_uri_validate(const char *uri, size_t capacity);
```

错误类型：

- `IOT_MQTT_URI_ERR_NULL`；
- `IOT_MQTT_URI_ERR_TOO_LONG`；
- `IOT_MQTT_URI_ERR_UNSUPPORTED_SCHEME`；
- `IOT_MQTT_URI_ERR_HOST`；
- `IOT_MQTT_URI_ERR_PORT`；
- `IOT_MQTT_URI_ERR_PATH`；
- `IOT_MQTT_URI_ERR_USERINFO`。

## 5. 测试

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Ifirmware/common/iot_product_config/include `
  firmware/common/iot_product_config/tests/test_iot_mqtt_uri_validation.c `
  firmware/common/iot_product_config/iot_mqtt_uri_validation.c `
  -o backups/tmp/iot-product-config-host/test_iot_mqtt_uri_validation.exe

.\backups\tmp\iot-product-config-host\test_iot_mqtt_uri_validation.exe
```

普通模式和 `-DNDEBUG` 模式均通过；测试同时覆盖显式容量、未终止输入、原始控制字符、percent-encoding 边界和大小写协议头。网关默认 OFF 构建也通过，最终网关 bin 仍为 `0x15f540`；模块尚未被 `main.c` 调用，因此镜像大小未变化。

## 6. 后续接入

1. SoftAP 请求解析层先读取 URI 到固定大小缓冲；
2. 先调用凭据长度/UTF-8 校验；
3. 再调用本 URI 校验；
4. 全部通过后才写入 NVS；
5. 硬件窗口验证连接成功、失败提示和旧配置保留；
6. TLS/WSS 等云配置等用户在场时按教学流程处理，不自动部署。
