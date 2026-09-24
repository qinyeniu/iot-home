# 电池传感器 SED 实验工程

状态：阶段 2，已加入 AHT20/BH1750 周期采集与 ZCL 上报；仅离线编译，**未烧录**。

## 目标

新增一个完全独立的电池供电 Zigbee 环境传感器工程，不修改现网常供电组合节点：

- 设备类型：Zigbee Sleepy End Device（SED）；
- 芯片：ESP32-C6；
- 信道：26；
- Endpoint：10；
- 默认 keep-alive / poll：60 秒；
- 启动目标发射功率：-10 dBm；
- 采样/上报周期：60 秒；
- 传感器：AHT20 温湿度、BH1750 光照。

## 为什么独立工程

现有 `firmware/node_zigbee` 集成 AHT20、BH1750、继电器和本地按键，并显式保持 `rx_on_when_idle=true`，按常供电和快速响应设计。把它直接改成低功耗会影响已经验证的远程开关链路，因此电池路线使用独立工程推进。

## 版本固定

`main/idf_component.yml` 精确固定：

- `espressif/esp-zboss-lib: 1.6.0`
- `espressif/esp-zigbee-lib: 1.6.0`

`dependencies.lock` 已纳入版本管理。烧录前仍需再次复核组件版本和构建产物，不能把 1.6.4/1.6.8 的临时解析结果误认为本工程目标版本。

## 阶段 2 数据格式

电池节点与现网 `node_zigbee` 保持同一套标准 ZCL 表示，网关无需修改：

| 指标 | Cluster | 类型与缩放 |
|---|---|---|
| 温度 | Temperature Measurement `0x0402` | `int16`，单位 0.01 °C |
| 湿度 | Relative Humidity Measurement `0x0405` | `uint16`，单位 0.01 % |
| 光照 | Illuminance Measurement `0x0400` | `uint16`，`10000 × log10(lux + 1)` |

每个采样周期按“温度 → 湿度 → 光照"发送三条标准 Report Attributes 单播帧，帧间隔 200 ms。目标为协调器短地址 `0x0000`、endpoint 10。

光照不能直接发送 BH1750 的 lux。网关按 Zigbee 对数刻度反算：

```text
lux = 10^(raw / 10000) - 1
```

## 低功耗与失败策略

- BH1750 使用 one-shot high-resolution 命令 `0x20`，转换结束后自动断电；不使用现网常供电节点的连续测量命令 `0x10`。
- AHT20 初始化只在启动或需要恢复时执行；测量完成后不主动写 Flash。
- 如果 ZCL/APS 请求在本地提交阶段被拒绝，最多立即重试 3 次。
- 如果帧已经发出但 APS 未确认，本阶段不做无线补报，等待下一个采样周期，以减少电量消耗和射频重试。
- AHT20/BH1750 任一读取失败时，会把对应本地 ZCL MeasuredValue 恢复为 invalid（温度 `0x8000`，湿度/光照 `0xFFFF`），避免网关主动 Read 时读到陈旧值；本阶段不主动发送 invalid report。
- 本工程不做 Flash 断网缓存。

## 离线编译

环境：ESP-IDF v5.5.4。

```powershell
. C:\Espressif\v5.5.4\esp-idf\export.ps1
idf.py build
```

阶段 2 结果：

```text
binary size 0x7d800 bytes
app partition 0xe1000 bytes
free 0x63800 bytes (44%)
```

阶段 1 结果（保留作对比）：

```text
binary size 0x74b80 bytes
free 0x6c480 bytes (48%)
```

## 当前行为

1. 启动 NVS 和 Power Management；
2. 配置 FreeRTOS tickless / light sleep；
3. 初始化 I2C 和 AHT20/BH1750 设备句柄；
4. 初始化 Zigbee ZED，关闭 idle 接收；
5. 注册 Basic、Identify、Temperature Measurement、Humidity Measurement、Illuminance Measurement clusters；
6. 工厂新设备执行 network steering；已配网设备尝试基于保存状态 rejoin；重试采用 5 秒 → 30 秒 → 60 秒退避；
7. 入网成功后日志打印 PAN、Extended PAN ID、channel 和 short address；
8. 每 60 秒读取一次 AHT20 与 BH1750，更新本地 ZCL 属性并向协调器发送三条报告；
9. 收到 `ESP_ZB_COMMON_SIGNAL_CAN_SLEEP` 时进入 Zigbee sleep。

## 硬件在线时的验收

必须由用户明确允许打开串口和烧录：

1. 确认目标端口，避免触发现网网关/节点误复位；
2. 复核组件版本为 1.6.0；
3. 只烧录该实验工程所在设备；
4. 验证能在 channel 26 加入目标 coordinator，而不是同信道其他开放网络；
5. 核对 PAN、Extended PAN ID、short address；
6. 验证网关每 60 秒收到温度、湿度、光照三项，且光照反算结果合理；
7. 验证空闲时进入 light sleep；
8. 用功率分析仪或低功耗电流表记录睡眠、轮询和上报电流；
9. 不擦 Zigbee NVRAM、不刷 bootloader/分区表，除非用户另行明确授权。

## 暂不做

- 不启用 Secure Boot / Flash encryption / NVS encryption；
- 不做 deep sleep；
- 不做 Flash 断网缓存；
- 不承诺即时下行命令；
- 不修改前后端和现网节点固件。

## 已知产品化限制

当前只固定 channel 26，尚未预置目标 Extended PAN ID、install code 或其他网络身份约束。因此在同信道存在多个开放网络时，仍可能加入非预期 coordinator。硬件测试时应确认 channel 26 上只有目标网络开放加入；后续产品化再选择预置 Extended PAN ID 或 install code 路线。

后续真正进入电池产品化前，还需要确认：AHT20/BH1750 的供电门控、I2C 上拉来源、板上 LED/USB 芯片功耗、keep-alive 与采样周期是否对齐，并用实物电流测试代替理论估算。