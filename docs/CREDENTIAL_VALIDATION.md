# 凭据输入校验模块

更新时间：2026-09-25  
状态：阶段 1a 已实现并通过主机测试；尚未接入网关启动、NVS 保存或 SoftAP 接口  
适用对象：ESP32-C6 网关

## 1. 目标

本地配网页面或未来的工厂配置工具在接收用户输入后，必须先确认字段：

- 指针和缓冲区容量有效；
- 必填字段非空；
- 字符串在产品长度限制内且正确 NUL 终止；
- 不包含换行、制表符、ASCII DEL、Unicode C1 等控制字符；
- MQTT 文本字段为合法 UTF-8；
- 校验失败时不打印、不回显用户输入的敏感内容。

本阶段只提供纯 C、可在开发机直接编译的校验函数，不改变现网固件行为。

## 2. 文件

```text
firmware/common/iot_product_config/include/iot_product_config_limits.h
firmware/common/iot_product_config/include/iot_credentials_validation.h
firmware/common/iot_product_config/iot_credentials_validation.c
firmware/common/iot_product_config/tests/test_iot_credentials_validation.c
```

长度常量被提取到独立头文件，主机测试和 ESP-IDF 组件复用同一组限制。

## 3. 当前校验规则

| 字段 | 是否必填 | 最大长度 | Wi-Fi 字节规则 | MQTT UTF-8 规则 |
|---|---|---:|---|---|
| Wi-Fi SSID | 是 | 32 | 允许高位字节 | — |
| Wi-Fi 密码 | 否 | 63 | 允许高位字节 | — |
| MQTT URI | 是 | 160 | — | 必须合法 UTF-8 |
| MQTT 用户名 | 否 | 64 | — | 必须合法 UTF-8 |
| MQTT 密码 | 否 | 64 | — | 必须合法 UTF-8 |
| MQTT Client ID | 否 | 64 | — | 必须合法 UTF-8 |

Wi-Fi 名称按 Zigbee/Wi-Fi 协议中的 32 字节 octet string 处理，因此不会简单拒绝原始高位字节；但所有字段都拒绝 ASCII C0 控制字符和 DEL。

MQTT 字段拒绝：

- 非法 UTF-8；
- overlong UTF-8；
- UTF-16 surrogate；
- 超过 U+10FFFF；
- 原始或 UTF-8 编码的 U+0080–U+009F C1 控制字符。

Wi-Fi 密码或 MQTT 用户名/密码为空字符串时，表示该项未配置；但 `NULL` 仍视为调用方参数错误，用来提醒上层先完成缓冲初始化。当前密码 API 是文本 API，不支持二进制密码；如果后续明确需要，应另建字节数组接口。

## 4. API

```c
iot_credentials_error_t iot_credentials_validate_text(
    const char *value,
    size_t capacity,
    size_t max_len,
    bool required,
    bool utf8,
    iot_credentials_field_t field,
    iot_credentials_field_t *failed_field);

iot_credentials_error_t iot_credentials_validate_wifi(
    const char *ssid,
    const char *password,
    iot_credentials_field_t *failed_field);

iot_credentials_error_t iot_credentials_validate_mqtt(
    const char *uri,
    const char *username,
    const char *password,
    const char *client_id,
    iot_credentials_field_t *failed_field);
```

失败原因包括：

- `IOT_CREDENTIALS_ERR_NULL`；
- `IOT_CREDENTIALS_ERR_EMPTY`；
- `IOT_CREDENTIALS_ERR_TOO_LONG`；
- `IOT_CREDENTIALS_ERR_CONTROL_CHAR`；
- `IOT_CREDENTIALS_ERR_INVALID_ENCODING`。

通用函数使用显式 `capacity`，校验器最多只读到 `capacity - 1` 的长度扫描边界，避免未来调用方传入恰好 MAX_LEN 字节的字段时发生一字节越界读。便捷的 Wi-Fi/MQTT 包装函数只适用于产品配置结构体等容量为 MAX_LEN + 1 的固定缓冲。

`failed_field` 可为 `NULL`；传入时会返回第一个失败字段，供本地页面显示“哪个字段不合法”，但上层不得把用户原始输入拼进日志。

## 5. 主机测试

不访问硬件、网络或服务器：

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Ifirmware/common/iot_product_config/include `
  firmware/common/iot_product_config/tests/test_iot_credentials_validation.c `
  firmware/common/iot_product_config/iot_credentials_validation.c `
  -o backups/tmp/iot-product-config-host/test_iot_credentials_validation.exe

.\backups\tmp\iot-product-config-host\test_iot_credentials_validation.exe
```

带 `-DNDEBUG` 的发布模式测试命令相同，只需额外加入 `-DNDEBUG`。

## 6. 当前不做

- 不接入 `firmware/gateway/main/main.c`；
- 不改变 legacy NVS key 或 A/B bundle 数据格式；
- 不改变前后端 API；
- 不启用 SoftAP、HTTP server、NVS encryption、TLS/WSS/VPN；
- 不自动清除现有配置；
- 不打印或落盘真实 Wi-Fi/MQTT 凭据。

## 7. 后续接入顺序

1. SoftAP 请求解析层把所有输入写入固定大小、零初始化的缓冲；
2. 调用本模块校验；
3. 仅在校验通过后调用现有 NVS 保存接口；
4. 增加 MQTT URI scheme/主机/端口校验；
5. 硬件窗口验证保存、断电、回滚和重新配网；
6. 最后才考虑关闭编译时凭据回退。

这样可以先保证输入边界安全，再逐步替换编译时写入凭据的实验流程。
