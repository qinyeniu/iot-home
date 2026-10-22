# ESP32-C6 双节点联动系统：GitHub 调研可信度评估 + 详细购买清单（2026-10-22）

> 本文档基于对 GitHub 实际仓库的 API 验证（star/更新时间/许可/代码结构），
> 评估"检测节点 Node A（雷达+BLE 存在检测）+ 控制节点 Node B（声光/红外/音频）"项目的可行性，
> 并给出可直接下单的详细购买清单。

---

## 一、GitHub 项目可信度评估（实测验证）

### 1.1 检测节点方向

| 项目 | Star | 更新 | 许可 | 可信度 | 借鉴点 |
|------|------|------|------|--------|--------|
| **xyzroe/Q_sensor** | 25 | 2026-08 | CC BY-NC-SA 4.0 | ⭐⭐⭐⭐⭐ | ESP32-C6 Zigbee 多传感器，**传感器组合与你的想法几乎完全一致**：AHT20(0x38)+BH1750(0x23)+BMP280(0x77)+QMI8658C 六轴(0x6b)+BS5820 5.8G 雷达(GPIO4)。雷达走标准 IAS Zone occupancy cluster 上报，有 zigbee2mqtt external converter，有 release 固件。 |
| **hubertciebiada/open-ambient-sensor** | 41 | 2026-10 | GPL-3.0 | ⭐⭐⭐⭐ | ESP32-C6 + HLK-LD2410C 雷达 + 环境传感器，v0.54 已量产验证（JLCPCB），ESPHome 方案。**证明 ESP32-C6 + LD2410 雷达可行**。 |
| **BegoonLab/xiao-esp32c6-wifi-sensor** | 65 | 2026-10 | — | ⭐⭐⭐⭐ | XIAO ESP32C6 电池供电+深睡+电池电压监测+MQTT，ESP-IDF v5.5.2。电池方案参考。 |
| **andi38/esp-zigbee-sensor-custom-cluster-reporting** | 1 | 2026-01 | — | ⭐⭐⭐ | ESP32-C6/H2 Zigbee 自定义 cluster(0xFF00) 上报 + light sleep，Tasmota 实测。**自定义 cluster 联动参考**。 |
| **Gustbel/ble-beacons-scanner_esp-idf** | 7 | 2024-12 | — | ⭐⭐ | ESP32 BLE iBeacon 扫描（ESP-IDF 例程改造）。**只做参考，BLE 部分官方例程更可靠**。 |

### 1.2 控制节点方向

| 项目 | Star | 更新 | 可信度 | 借鉴点 |
|------|------|------|--------|--------|
| **virsavp/ESP32-Home-Assistant-ir-blaster** | 6 | 2026-07 | ⭐⭐⭐ | ESP32-S3 红外发射（硬件设计完成，固件待补）。红外发射管+驱动参考。 |
| **Vourhey/night-light** | 0 | 2026-03 | ⭐⭐ | ESP32-C6 Zigbee RGB 灯（WS2812B）。 |
| **YevheniiMelikov/ESP-ZB-Dimmable-light-with-pwm-fade** | 0 | 2025-08 | ⭐⭐ | ESP32-C6 Zigbee 控制 LED（PWM fade）。 |

### 1.3 关键：BLE + Zigbee 共存可行性（最大技术风险）

**结论：可行，乐鑫官方支持。**

- **ESP-IDF 内置 `esp_coex` 组件**，明确支持 **ESP32-C6**（`components/esp_coex/esp32c6/esp_coex_adapter.c`）。
- **乐鑫官方 issue #316**（esp-zigbee-sdk）：用户用 ESP32-C6 协调器 + ESP32-H2 终端设备同时跑 BLE NIMBLE + Zigbee 时崩溃。
  - 官方回复：根因是**多任务未加锁调用 SDK API**，要求使用 `esp_zb_lock_acquire` / `esp_zb_lock_release` 保护。
  - 用户按官方建议加锁后问题解决。
- **乐鑫员工提供官方测试例程 `lpy4105/esp-zigbee-sdk` 的 `test/ble_zb_light` 分支**：BLE + Zigbee 灯同时工作。

**对我们的启示**：
- Node A 的 BLE 扫描 + Zigbee 上报**可以共存**，但必须：
  1. BLE 扫描做成短时突发（如每次 2-3 秒），避免长期占用射频；
  2. Zigbee SDK API 调用统一加 `esp_zb_lock_*` 保护；
  3. 参考官方 `ble_zb_light` 例程的结构。

---

## 二、技术可行性结论

| 功能 | 可行性 | 依据 |
|------|--------|------|
| ESP32-C6 跑 Zigbee 多传感器 | ✅ 高 | Q_sensor 已实现，传感器组合几乎一致 |
| 5.8G/24G 雷达人体存在 | ✅ 高 | Q_sensor 用 BS5820（GPIO 中断 + IAS Zone）；OAS 用 LD2410C |
| 六轴姿态（MPU6050/QMI8658C） | ✅ 高 | Q_sensor 用 QMI8658C；ESP-IDF 有 MPU6050 驱动（nopnop2002 等） |
| BLE 扫描绑定手机 MAC | ✅ 高 | ESP-IDF 官方 BLE scanner 例程；BLE+Zigbee 共存有官方支持 |
| **BLE+Zigbee 同时工作** | ✅ 中高 | 官方支持，需加锁 + 分时扫描 |
| Zigbee 自定义 cluster 本地联动 | ✅ 中高 | andi38 项目验证自定义 cluster 上报；节点间写属性回调可参考 |
| 红外发射控制空调/电视 | ✅ 高 | 成熟方案（IRremote ESP-IDF / virsavp 项目） |
| MAX98357A I2S 音频 | ✅ 高 | ESP-IDF I2S 例程成熟（liux-pro/EPS32-S3-I2S-MAX98357 等） |
| 电池供电 + 常开 | ✅ 高 | MT3608 常开升压（已在清单）；BegoonLab 深睡方案参考 |

**综合可行性评级：⭐⭐⭐⭐☆（4.5/5）**
- 主要不确定性：BLE+Zigbee 共存需要仔细处理（加锁+分时），以及自定义 cluster 需要网关侧配合解析。

---

## 三、详细购买清单

### 3.1 检测节点 Node A（Sensor Hub）

| # | 物品 | 淘宝搜索词 | 数量 | 参考价/个 | 小计 | 用途 | 接线 |
|---|------|-----------|------|----------|------|------|------|
| 1 | **ESP32-C6 SuperMini**（已有） | — | 1 | 已有 | 0 | 主板 | — |
| 2 | **AHT20 温湿度模块**（已有） | — | 1 | 已有 | 0 | 温湿度 | I2C (SDA/SCL) |
| 3 | **BH1750 光照模块**（已有） | — | 1 | 已有 | 0 | 光照 | I2C |
| 4 | **BMP280 气压模块** | 「BMP280 气压模块 3.3V」 | 1 | ~3 元 | 3 | 气压 | I2C (0x77) |
| 5 | **MPU6050 六轴模块** | 「MPU6050 模块 GY-521」 | 1 | ~5 元 | 5 | 六轴/跌倒 | I2C (0x68) |
| 6 | **HLK-LD2410C 5.8G/24G 雷达模块** | 「HLK-LD2410C 雷达模块」 | 1 | ~15-20 元 | 18 | 人体存在 | UART TX/RX + 5V/GND（注意：LD2410 需 5V，用 MT3608 输出） |
| 7 | **0.96" OLED SSD1306 I2C** | 「0.96 OLED SSD1306 I2C」 | 1 | ~8 元 | 8 | 本地显示 | I2C (0x3C) |
| 8 | **有源蜂鸣器 5V** | 「有源蜂鸣器 5V 模块」 | 1 | ~1.5 元 | 1.5 | 本地提示 | GPIO + 5V |
| 9 | **轻触按键 6x6** | 「轻触按键 6x6 模块」 | 2 | ~0.5 元 | 1 | 交互/配对 | GPIO |
| 10 | **MT3608 升压模块**（已在清单） | 「MT3608 升压模块 5V」 | 1 | ~3 元 | 3 | 电池→5V 常开 | IN+ 电池+/IN- GND；OUT+ 5V |
| 11 | **TP4056 电池盒 + 18650**（已有） | — | 1 | 已有 | 0 | 供电 | 电池盒红黑线→MT3608 IN |
| 12 | **杜邦线/面包板** | 「杜邦线 公对母 母对母 面包板」 | 若干 | ~5 元 | 5 | 接线 | — |

**Node A 小计：约 44.5 元**（不含已有）

### 3.2 控制节点 Node B（Actuator Hub）

| # | 物品 | 淘宝搜索词 | 数量 | 参考价/个 | 小计 | 用途 | 接线 |
|---|------|-----------|------|----------|------|------|------|
| 1 | **ESP32-C6 SuperMini** | 「ESP32-C6 SuperMini 开发板」 | 1 | ~14-18 元 | 16 | 主板 | — |
| 2 | **继电器模块 5V 低电平触发** | 「继电器模块 5V 低电平 1路」 | 1 | ~3 元 | 3 | 控制外部设备 | IN→GPIO；VCC→5V；GND |
| 3 | **有源蜂鸣器 5V** | 「有源蜂鸣器 5V 模块」 | 1 | ~1.5 元 | 1.5 | 报警音 | GPIO + 5V |
| 4 | **RGB LED 模块 共阴** | 「RGB LED 模块 共阴 3色」 | 1 | ~2 元 | 2 | 状态指示 | R/G/B→GPIO；GND |
| 5 | **红外发射管 + 接收管套装** | 「红外发射管 940nm 接收管 套装」 | 1 | ~3 元 | 3 | 红外控制 | 发射管→GPIO（经三极管）；接收管→GPIO |
| 6 | **MAX98357A I2S 音频模块 + 小喇叭**（可选） | 「MAX98357A 喇叭模块 3W」 | 1 | ~10 元 | 10 | 语音/警笛 | BCLK/LRC/DIN→GPIO；VIN→3.3V；SPK±→喇叭 |
| 7 | **0.96" OLED SSD1306 I2C** | 「0.96 OLED SSD1306 I2C」 | 1 | ~8 元 | 8 | 显示报警来源 | I2C |
| 8 | **轻触按键 6x6** | 「轻触按键 6x6 模块」 | 2 | ~0.5 元 | 1 | 本地确认/消警 | GPIO |
| 9 | **MT3608 升压模块**（已在清单） | 「MT3608 升压模块 5V」 | 1 | ~3 元 | 3 | 电池→5V 常开 | 同上 |
| 10 | **TP4056 电池盒 + 18650**（已有） | — | 1 | 已有 | 0 | 供电 | 同上 |
| 11 | **杜邦线/面包板** | 「杜邦线 面包板」 | 若干 | ~5 元 | 5 | 接线 | — |

**Node B 小计：约 52.5 元**（不含可选音频则约 42.5 元）

### 3.3 两节点合计

| 项目 | 金额 |
|------|------|
| Node A 新增 | ~44.5 元 |
| Node B 新增（含音频） | ~52.5 元 |
| **合计** | **约 97 元**（不含音频约 87 元） |
| 已复用硬件 | ESP32-C6 SuperMini×1、AHT20、BH1750、IP5310×2、TP4056 电池盒×2、18650×2、MT3608×2（待购） |

> ⚠️ 注意：需要再买 **1 颗 ESP32-C6 SuperMini**（Node B 用），因为现有那颗要用于 Node A。

---

## 四、引脚分配建议（ESP32-C6 SuperMini）

### Node A（检测节点）
| 外设 | 引脚 |
|------|------|
| I2C SDA（AHT20/BH1750/BMP280/MPU6050/OLED 共用） | GPIO6 |
| I2C SCL | GPIO7 |
| 雷达 UART TX | GPIO16 |
| 雷达 UART RX | GPIO17 |
| 蜂鸣器 | GPIO18 |
| 按键 1（配对） | GPIO19 |
| 按键 2（显示切换） | GPIO20 |
| LED 状态 | GPIO21 |

### Node B（控制节点）
| 外设 | 引脚 |
|------|------|
| I2C SDA（OLED） | GPIO6 |
| I2C SCL | GPIO7 |
| 继电器 IN | GPIO8 |
| 蜂鸣器 | GPIO9 |
| RGB R | GPIO10 |
| RGB G | GPIO11 |
| RGB B | GPIO12 |
| 红外发射 | GPIO13 |
| 按键 1（消警） | GPIO14 |
| 按键 2（测试） | GPIO15 |

> 注：SuperMini 的 GPIO0/1/2/3/4/5 也可用，但 GPIO0/1 通常为下载/调试相关，建议避开；GPIO4/5 若不用可做通用 IO。

---

## 五、风险与替代方案

| 风险 | 等级 | 对策 |
|------|------|------|
| BLE+Zigbee 共存干扰 | 中高 | 短时扫描（2-3s）+ Zigbee API 加锁 + 参考官方 ble_zb_light |
| 自定义 cluster 网关不识别 | 中 | 网关增加私有 cluster 透传/解析（现有网关固件可扩展） |
| LD2410 需 5V | 低 | 用 MT3608 5V 输出供电；雷达 GND 与 ESP32 共地 |
| MPU6050 模块 3.3V 兼容 | 低 | GY-521 模块自带稳压，直接 3.3V 可用 |
| 红外码学习复杂 | 中 | 先预存常见码（空调开关/电视开关），后续加学习 |
| MAX98357A I2S 调试 | 低 | 先用蜂鸣器做声响应，音频进阶 |
| 电池容量/续航 | 低 | 18650+MT3608 常开，Node A 若深睡可大幅延长 |

---

## 六、结论

- **项目可信**：核心参考 Q_sensor（25★）和 open-ambient-sensor（41★）都真实存在且持续更新；BLE+Zigbee 共存有乐鑫官方支持。
- **项目可行**：传感器组合、雷达、BLE、自定义 cluster 均有现成参考；主要工作量在"雷达+BLE 存在检测联动"和"自定义 cluster 节点间通信"两块自有逻辑。
- **建议采购**：按第三节清单购买，先做 Node A（检测）验证传感器和雷达，再做 Node B（控制），最后打通联动。
- **预算**：约 87-97 元（不含已有硬件）。

---

*关联文档：*
- `docs/ESP32C6_DUAL_NODE_SYSTEM_PLAN_2026-10-22.md`（双节点联动系统计划）
- `docs/ESP32C6_MULTI_NODE_PLAN_2026-10-22.md`（单节点多功能方案）
- `docs/HARDWARE_PURCHASE_AND_BATTERY_PLAN_2026-10-19.md`（硬件采购与电池供电计划）
