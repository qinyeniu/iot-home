# ESP32-C6 双节点联动系统计划（2026-10-22）

> 用户思路：一个检测节点（环境+人体+BLE），一个控制节点（声光/红外/音频执行），
> 两节点通过 Zigbee 本地联动（**不依赖 WiFi/云**），同时经网关联网（MQTT/FastAPI）。

## 0. 系统总览
```
[检测节点 Node A]──Zigbee 本地联动──>[控制节点 Node B]
      │                                    │
      └────────── Zigbee ──> [网关] ── WiFi/MQTT ──> FastAPI/MySQL
```
- **本地联动**：Node A 检测到异常（人存在但绑定手机不在附近 / 跌倒 / 环境超标）→ 通过 Zigbee 直接发命令给 Node B → Node B 声光报警/红外控制/音频响应。
- **云端联网**：两个节点的数据都经网关上报，远程可查看、可下发控制。
- **不受 WiFi 影响**：Node A→Node B 的联动走 Zigbee 局域网，即使 WiFi/云断开也能工作。

## 1. 节点 A：检测节点（Sensor Hub）
**硬件**：
| 模块 | 类型 | 用途 |
|------|------|------|
| AHT20（已有） | 温湿度 | 环境监测 |
| BH1750（已有） | 光照 | 环境监测 |
| BMP280（新增） | 气压 | 气压/高度 |
| MPU6050（新增） | 六轴 | 跌倒/倾斜检测 |
| 5.8G 雷达（新增） | 人体存在 | 有人/无人 |
| OLED 0.96"（新增） | 本地显示 | 显示数据/状态 |
| 蜂鸣器（新增） | 本地报警 | 本地提示（可选） |
| 按键（新增） | 交互 | 切换显示/布撤防 |

**核心创新：BLE 存在检测**
- ESP32-C6 自带 BLE。
- 绑定 1-N 个手机蓝牙 MAC（预配置）。
- 当雷达检测到**有人**时，启动 BLE 扫描：
  - 能扫描到绑定设备 → 正常（不报警）。
  - **扫描不到绑定设备**（陌生人）→ 通过 Zigbee 通知 Node B 报警。
- 周期/事件触发扫描，避免常开 BLE 耗电。

## 2. 节点 B：控制节点（Actuator Hub）
**硬件**：
| 模块 | 类型 | 用途 |
|------|------|------|
| 继电器模块（新增） | 执行器 | 控制 220V 设备/灯 |
| 蜂鸣器（新增） | 声 | 报警音 |
| RGB LED（新增） | 光 | 状态指示（红=报警，绿=正常） |
| 红外发射管/模块（新增） | 红外 | 控制空调/电视等红外设备 |
| 音频播放（可选） | 音频 | 语音提示/警笛 |
| OLED 0.96"（可选） | 本地显示 | 显示报警来源/状态 |
| 按键（可选） | 交互 | 本地确认/消警 |

**执行逻辑（本地联动）**：
- 收到 Node A 报警命令 → 继电器动作 + 蜂鸣器响 + RGB 红闪。
- 收到 Node A 解除命令 → 继电器复位 + 蜂鸣器停 + RGB 绿。
- 红外控制：本地学习/预存红外码，收到命令后发射（如开灯/关空调）。

## 3. 本地 Zigbee 联动协议（自定义 cluster）
- 节点间通信：ESP32-C6 Zigbee 支持 **end-device binding / 自定义 cluster**。
- 参考：`andi38/esp-zigbee-sensor-custom-cluster-reporting`（自定义 cluster 0xFC00 上报）。
- 定义私有 cluster（如 0xFC01）：
  - **属性 0x0000**：命令（1=报警，2=解除，3=红外发射，4=RGB 设置）。
  - **属性 0x0001**：来源节点地址。
  - **属性 0x0002**：附加数据（如红外码、RGB 值）。
- Node A 写 cluster → Node B 收到写属性回调 → 执行动作。
- 同时经网关把事件上报云端（MQTT）。

## 4. 云端联网（复用现有链路）
- Node A/B 都作为 Zigbee 子节点接入现有网关（信道 26）。
- 传感器数据 → 网关 → MQTT `nodes/zb-XXXX/telemetry`。
- 报警事件 → 网关 → MQTT `nodes/gw-001/status`（`source:zigbee`）。
- 远程控制 → 网关本地菜单 / MQTT 下发 → 节点执行。

## 5. 采购清单（两节点合计，约 60-90 元）
| # | 物品 | 搜索词 | 数量 | 参考价 | 用在哪 |
|---|------|--------|------|--------|-------|
| 1 | BMP280 气压模块 | 「BMP280 气压模块」 | 1 | ~3 元 | A |
| 2 | MPU6050 六轴模块 | 「MPU6050 模块」 | 1 | ~5 元 | A |
| 3 | 5.8G 人体雷达 | 「HLK-LD2410 5.8G 雷达」 | 1 | ~15 元 | A |
| 4 | OLED 0.96" SSD1306 | 「0.96 OLED SSD1306」 | 2 | ~16 元 | A+B |
| 5 | 有源蜂鸣器 5V | 「有源蜂鸣器 5V」 | 2 | ~2 元 | A+B |
| 6 | RGB LED 模块 | 「RGB LED 模块 共阴」 | 1 | ~2 元 | B |
| 7 | 继电器模块（低电平触发） | 「继电器模块 5V 低电平」 | 1 | ~3 元 | B |
| 8 | 红外发射模块/管 | 「红外发射管 940nm 模块」 | 1 | ~3 元 | B |
| 9 | 轻触按键 | 「轻触按键 6x6」 | 4 | ~2 元 | A+B |
| 10 | MAX98357A I2S 音频放大 + 小喇叭（可选） | 「MAX98357A 喇叭模块」 | 1 | ~10 元 | B |
| 11 | MT3608 升压模块（已在清单） | 「MT3608 升压模块」 | 2 | ~6 元 | A+B 供电 |

**总新增预算：约 60-70 元**（不含已有 AHT20/BH1750/电池/TP4056/MT3608）。

## 6. GitHub 开源参考
| 项目 | 地址 | 借鉴点 |
|------|------|--------|
| **xyzroe/Q_sensor** | https://github.com/xyzroe/Q_sensor | ESP32-C6 Zigbee 多传感器（AHT20+BH1750+BMP280+姿态+雷达），标准 ZCL 上报 |
| **andi38/esp-zigbee-sensor-custom-cluster-reporting** | https://github.com/andi38/esp-zigbee-sensor-custom-cluster-reporting | ESP32-C6 自定义 cluster 上报（0xFC00） |
| **h2okopfmt/esp32c6-window-sensor** | https://github.com/h2okopfmt/esp32c6-window-sensor | ESP32-C6 Zigbee 自定义 cluster（0xFC00） |
| **Gustbel/ble-beacons-scanner_esp-idf** | https://github.com/Gustbel/ble-beacons-scanner_esp-idf | ESP32 BLE iBeacon 扫描（存在检测） |
| **BegoonLab/xiao-esp32c6-wifi-sensor** | https://github.com/BegoonLab/xiao-esp32c6-wifi-sensor | 电池供电+深睡+电池电压监测 |
| **virsavp/ESP32-Home-Assistant-ir-blaster** | https://github.com/virsavp/ESP32-Home-Assistant-ir-blaster | ESP32 红外发射（MQTT 控制） |
| **Vourhey/night-light** | https://github.com/Vourhey/night-light | ESP32-C6 Zigbee RGB 灯（WS2812B） |
| **YevheniiMelikov/ESP-ZB-Dimmable-light-with-pwm-fade** | https://github.com/YevheniiMelikov/ESP-ZB-Dimmable-light-with-pwm-fade | ESP32-C6 Zigbee 控制 LED |

## 7. 里程碑
- **M1**：采购到货，两个节点面包板搭建。
- **M2**：驱动移植（AHT20/BH1750/BMP280/MPU6050/雷达/OLED/蜂鸣器/RGB/继电器/红外）。
- **M3**：Node A 入网 + 传感器上报。
- **M4**：Node B 入网 + 执行器本地测试。
- **M5**：Zigbee 自定义 cluster 联动（A→B 报警/解除）。
- **M6**：BLE 存在检测（绑定手机 MAC，陌生人报警）。
- **M7**：红外控制 + 音频响应（可选）。
- **M8**：云端接入（MQTT 上报 + 远程控制）。
- **M9**：低功耗优化 + 电池供电实测。
- **M10**：文档 + 提交。

## 8. 技术风险与对策
| 风险 | 对策 |
|------|------|
| ESP32-C6 BLE+Zigbee 共存干扰 | 分时复用（BLE 扫描短暂，Zigbee 上报错开）；参考 ESP-IDF 共存例程 |
| 自定义 cluster 与网关兼容 | 网关需能透传/识别私有 cluster（可先在网关加解析） |
| 雷达功耗较高 | 低功耗模式只在需要时唤醒；或 GPIO 控制雷达供电 |
| MPU6050 供电电压 | 确认模块 3.3V 兼容（多数带稳压） |
| 红外码学习复杂 | 先预存常见码（如空调开关），后续再做学习 |
| 音频 I2S 调试复杂 | 先用蜂鸣器做声响应，音频作为可选进阶 |

## 9. 与现有项目关系
- 完全复用现有 Zigbee 网络（信道 26、网关）。
- 复用 node_zigbee/node_battery_sensor 的上报框架。
- 网关本地控制菜单可显示两个节点。
- 新增自定义 cluster 联动逻辑，需要网关侧配合解析/透传。

## 10. 待确认问题（需用户决策）
1. **音频响应**：是否真的要加 MAX98357A+喇叭（+10 元）？还是先只用蜂鸣器？
2. **红外控制**：是否要加红外发射（控制电视/空调）？还是先不加？
3. **绑定 BLE 设备**：手机蓝牙 MAC 如何配置？（烧录时写死 / 通过网关 MQTT 下发 / 按键配对）
4. **继电器用途**：控制什么设备？（灯 / 风扇 / 220V 电器）
5. **电池供电**：两个节点都用电池（MT3608 常开），还是一个用电池、一个用 USB？
