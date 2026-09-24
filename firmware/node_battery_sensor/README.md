# 电池传感器 SED 实验工程

状态：阶段 1 骨架；仅离线编译，**未烧录**。

## 目标

新增一个完全独立的电池供电 Zigbee 环境传感器工程，不修改现网常供电组合节点：

- 设备类型：Zigbee Sleepy End Device（SED）；
- 芯片：ESP32-C6；
- 信道：26；
- Endpoint：10；
- 默认 keep-alive / poll：60 秒；
- 启动目标发射功率：-10 dBm；
- 当前阶段只完成入网、端点和 light sleep 骨架。

后续阶段再加入 AHT20、BH1750、周期采样和 ZCL attribute report。

## 为什么独立工程

现有 `firmware/node_zigbee` 集成 AHT20、BH1750、继电器和本地按键，并显式保持 `rx_on_when_idle=true`，按常供电和快速响应设计。把它直接改成低功耗会影响已经验证的远程开关链路，因此电池路线使用独立工程推进。

## 版本固定

`main/idf_component.yml` 精确固定：

- `espressif/esp-zboss-lib: 1.6.0`
- `espressif/esp-zigbee-lib: 1.6.0`

`dependencies.lock` 已纳入版本管理。烧录前仍需再次复核组件版本和构建产物，不能把 1.6.4/1.6.8 的临时解析结果误认为本工程目标版本。

## 离线编译

环境：ESP-IDF v5.5.4。

```powershell
. C:\Espressif\v5.5.4\esp-idf\export.ps1
idf.py set-target esp32c6
idf.py build
```

阶段 1 结果：

```text
binary size 0x74b80 bytes
app partition 0xe1000 bytes
free 0x6c480 bytes (48%)
```

## 当前行为

1. 启动 NVS 和 Power Management；
2. 配置 FreeRTOS tickless / light sleep；
3. 初始化 Zigbee ZED；
4. 关闭 idle 接收；
5. 注册 Basic、Identify、Temperature Measurement、Humidity Measurement、Illuminance Measurement clusters；
6. 工厂新设备执行 network steering；已配网设备尝试基于保存状态 rejoin；重试采用 5 秒 → 30 秒 → 60 秒退避；
7. 入网成功日志打印 Extended PAN ID，用于核对是否加入目标网络；
8. 收到 `ESP_ZB_COMMON_SIGNAL_CAN_SLEEP` 时进入 Zigbee sleep。

## 硬件在线时的验收

必须由用户明确允许打开串口和烧录：

1. 确认目标端口，避免触发现网网关/节点误复位；
2. 复核组件版本为 1.6.0；
3. 只烧录该实验工程所在设备；
4. 验证能在 channel 26 加入现有 coordinator；
5. 验证 short address 非 `0x0000/0xFFFE/0xFFFF`；
6. 验证空闲时进入 light sleep；
7. 用功率分析仪或低功耗电流表记录睡眠电流；
8. 不擦 Zigbee NVRAM、不刷 bootloader/分区表，除非用户另行明确授权。

## 暂不做

- 不启用 Secure Boot / Flash encryption / NVS encryption；
- 不做 deep sleep；
- 不做 Flash 断网缓存；
- 不承诺即时下行命令；
- 不修改前后端和现网节点固件。

## 已知产品化限制

当前只固定 channel 26，尚未预置目标 Extended PAN ID、install code 或其他网络身份约束。因此在同信道存在多个开放网络时，仍可能加入非预期 coordinator。硬件测试时应确认 channel 26 上只有目标网络开放加入；后续产品化再选择预置 Extended PAN ID 或 install code 路线。


