# iot_rf_packet

私有 RF 功率包的纯逻辑编解码组件。组件只使用 C 标准库，不依赖 ESP-IDF、Zigbee、FreeRTOS 或硬件接口，可在电脑主机上直接编译和测试。

## 线格式

使用固定字节布局，没有多字节字段；`power_dbm` 使用有符号 `int8_t`。

| Byte | V1 (6 bytes) | V2 (8 bytes) |
| ---: | --- | --- |
| 0 | magic0 = `0x52` (`'R'`) | 同 V1 |
| 1 | magic1 = `0x46` (`'F'`) | 同 V1 |
| 2 | version = `1` | version = `2` |
| 3 | command | command |
| 4 | sequence | sequence |
| 5 | power_dbm（有符号） | power_dbm（有符号） |
| 6 | 不存在 | mode |
| 7 | 不存在 | reserved，必须为 `0` |

命令：

- `IOT_RF_CMD_QUERY = 1`
- `IOT_RF_CMD_SET = 2`
- `IOT_RF_CMD_REPORT = 3`

V2 模式：

- `IOT_RF_MODE_AUTO = 0`
- `IOT_RF_MODE_MANUAL = 1`

V1 没有 mode 字节；解码后 `mode` 被设置为 `IOT_RF_MODE_UNKNOWN`，编码 V1 时调用方传入的 mode 会被忽略。

## API

- `iot_rf_packet_wire_length()`：返回指定版本的线格式长度；
- `iot_rf_packet_command_is_valid()`：判断命令是否受支持；
- `iot_rf_packet_mode_is_valid()`：判断 V2 模式是否受支持；
- `iot_rf_packet_decode()`：校验并解码完整私有包；
- `iot_rf_packet_encode()`：按调用方提供的数据生成 V1 或 V2 私有包。

解码要求调用方已经取得一整个帧，并提供精确长度。组件不会在字节流中搜索 magic，也不会自行处理粘包/分包。

## 主机测试

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File firmware/common/iot_rf_packet/test/run_host_test.ps1
```

测试覆盖 V1/V2 编解码、负功率、非法 magic/版本/命令/长度、V2 mode 和 reserved 校验，以及空指针和输出缓冲不足等边界。
