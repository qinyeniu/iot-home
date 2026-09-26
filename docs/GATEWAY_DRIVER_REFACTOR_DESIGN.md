# 网关驱动拆分与基础驱动类型设计

日期：2026-09-26
适用固件：`firmware/gateway`
状态：阶段 A/B/C/D 已完成（阶段 C/D 随本次提交入库）。基础驱动类型、可复用 SSD1306 组件、USB Serial/JTAG 传输组件、Wi‑Fi STA 基础连接组件和普通 Wi‑Fi 连接服务已落地；默认/实验构建通过，最终默认固件 75 秒硬件短测 PASS。

## 1. 背景与目标

当前网关主要逻辑集中在 `firmware/gateway/main/main.c`，单文件约 5600 行，混合了 OLED、Wi‑Fi、MQTT、Zigbee 协调、RF 功率策略、私有协议编解码、任务和启动装配。继续在单文件中开发会带来：

- 硬件驱动难以在网关、节点或后续板型之间复用；
- 业务策略与 I2C/GPIO 等底层细节耦合，测试和复审成本升高；
- 修改一个驱动时容易碰到无关业务逻辑；
- 新成员或后续会话难以快速判断代码边界。

本次重构目标：

1. 建立一个简单、可复用、适合 C/嵌入式场景的基础驱动生命周期类型；
2. 先把边界最清晰的 SSD1306 OLED 驱动拆出；
3. 不引入动态分配，不改变现有硬件行为和启动顺序；
4. 后续按模块逐步拆分，每次保持可编译、可短测、可单独回滚；
5. `main.c` 最终只保留应用装配、任务入口和少量跨模块胶水逻辑。

## 2. 分层原则

### 2.1 驱动层（drivers / BSP）

负责具体硬件资源，例如：

- I2C / SSD1306 OLED；
- GPIO、LED、按键；
- 后续可抽离的 NVS 存储访问、板级复位与健康计数。

驱动层不能依赖 MQTT、Wi‑Fi 业务状态或 Zigbee 业务流程；硬件事件通过返回值、回调或轻量事件上报。

### 2.2 服务与策略层（services）

负责不直接属于某颗芯片的逻辑，例如：

- Wi‑Fi 连接所有权与重连策略；
- MQTT 客户端、消息上下文和断网 outbox；
- RF 私有协议编解码；
- RF 发射功率策略、后台控制器、命令队列；
- Zigbee 节点模型与事件分发。

### 2.3 任务与装配层（tasks / app）

负责 FreeRTOS 任务、事件队列和模块初始化顺序：

- `app_main` 只做硬件与服务装配；
- 各任务循环调用对应服务；
- Zigbee、MQTT、Wi‑Fi 回调只做短处理，再把事件交给任务上下文。

依赖方向固定为：装配层 → 服务层 → 驱动层；驱动层不得反向调用业务层。

## 3. 基础驱动类型

在通用组件 `iot_driver_base` 中提供最小生命周期接口：

```c
typedef esp_err_t (*iot_driver_lifecycle_fn)(void *context);

typedef struct {
    const char *name;
    iot_driver_lifecycle_fn init;
    iot_driver_lifecycle_fn start;
    iot_driver_lifecycle_fn stop;
    iot_driver_lifecycle_fn deinit;
    iot_driver_lifecycle_fn poll;
} iot_driver_t;
```

约定：

- `init`：一次性分配/初始化底层资源，例如 I2C 总线；
- `start`：让设备进入工作状态，例如点亮面板并写入初始化命令；
- `stop`：停止外设工作，但保留总线资源；
- `deinit`：释放底层资源；
- `poll`：执行周期维护或刷新；
- 未使用的回调允许为 `NULL`，基础辅助函数按 `ESP_OK` 处理；
- 所有实例和帧缓冲仍由调用方静态提供，驱动内部不做 `malloc()`。

基础辅助函数：

```c
esp_err_t iot_driver_init(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_start(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_stop(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_deinit(const iot_driver_t *driver, void *context);
esp_err_t iot_driver_poll(const iot_driver_t *driver, void *context);
```

该接口不追求覆盖所有复杂框架，只统一常见启动/停止/轮询方式；具体驱动仍提供强类型 API，避免业务代码为复用而大量使用 `void *`。

## 4. 首个组件：SSD1306 OLED

### 4.1 组件位置

新增通用组件：

```text
firmware/common/iot_ssd1306/
├── CMakeLists.txt
├── iot_ssd1306.c
└── include/iot_ssd1306.h
```

新增基础接口组件：

```text
firmware/common/iot_driver_base/
├── CMakeLists.txt
└── include/iot_driver.h
```

### 4.2 配置与实例

配置通过静态结构体传入：

```c
typedef struct {
    i2c_port_t i2c_port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint8_t i2c_address;
    uint32_t i2c_freq_hz;
    uint8_t *framebuffer;
    size_t framebuffer_size;
} iot_ssd1306_config_t;
```

实例对象保存帧缓冲配置、重试计数、恢复计数和初始化状态。帧缓冲由网关静态提供，大小为 1024 字节，避免组件内部动态分配。

### 4.3 强类型 API

计划提供：

```c
esp_err_t iot_ssd1306_init(iot_ssd1306_t *self, const iot_ssd1306_config_t *config);
esp_err_t iot_ssd1306_probe(iot_ssd1306_t *self, bool *present);
esp_err_t iot_ssd1306_panel_start(iot_ssd1306_t *self);
esp_err_t iot_ssd1306_recover(iot_ssd1306_t *self);
esp_err_t iot_ssd1306_clear(iot_ssd1306_t *self);
esp_err_t iot_ssd1306_draw_text(iot_ssd1306_t *self, int x, int y, const char *text);
esp_err_t iot_ssd1306_flush(iot_ssd1306_t *self);
const iot_driver_t *iot_ssd1306_driver(void);
```

原有 I2C 总线恢复、三次写入重试、分块刷新、失败后自动恢复并重试一次、5x7 ASCII 字体等行为保持不变。

## 5. 分阶段落地顺序

### 阶段 A：建立基础类型与 OLED 组件（本次）

1. 新增 `iot_driver_base`；
2. 新增实例化、无动态分配的 `iot_ssd1306`；
3. 从 `main.c` 删除 OLED 帧缓冲、字体和 I2C/SSD1306 实现；
4. `main.c` 保留板级引脚定义、静态实例和显示内容；
5. 更新网关组件依赖；
6. 构建默认 OFF 与实验 ON 配置；
7. 做一次硬件短测，确认屏幕点亮和持续刷新正常。

### 阶段 B：继续抽离驱动

候选：

- 板级 LED / 按键；
- NVS 键值访问封装；
- Zigbee 原始 APS 适配（作为协议适配而不是业务服务）。

### 阶段 C：抽离服务

候选：

- MQTT outbox 与消息上下文；
- Wi‑Fi 所有权、重连与 RF 健康扫描；
- RF 协议编解码；
- RF 功率策略与后台控制器；
- Zigbee 节点模型和事件转发。

每阶段只搬一个职责，默认不改变行为；每个阶段完成后更新文档并提交。

## 6. 验证标准与风险控制

本设计阶段 A 的验证标准：

- 网关默认 OFF 与实验 ON 构建均通过；
- 未读取或提交任何密钥文件；
- OLED 初始化日志和启动画面与原行为一致；
- 屏幕持续刷新，无明显花屏、闪烁或持续恢复日志；
- Zigbee、Wi‑Fi、MQTT 不因 OLED 拆分出现异常复位；
- 4 小时夜间长测仍以 raw APS buffer 泄漏修复为核心：成功发送数显著超过 33，且入口 `ESP_ERR_NO_MEM=0`；`status=89` 单独统计，不等同于 buffer 泄漏。

风险控制：

- 不擦 Flash、不擦 Zigbee NVRAM、不刷 bootloader/分区表；
- 只在需要上板验证时打开串口或烧录应用区，并提前告知串口可能触发复位；
- 每次显式添加相关文件，不使用 `git add -A`；
- 重构提交与修复、测试证据提交分开，便于回滚。


## 7. 阶段 A 实施记录（2026-09-26）

已新增：

- `firmware/common/iot_driver_base`：最小驱动生命周期接口和 NULL hook 兼容辅助函数；
- `firmware/common/iot_ssd1306`：实例化、无动态分配的 SSD1306 128x64 文本驱动。

`main.c` 仍拥有板级引脚、静态实例、1024 字节帧缓冲和显示内容；I2C/SSD1306 的初始化、探测、总线恢复、字体、分块刷新等底层逻辑已移至共享组件。

构建结果：

- 默认 OFF：`firmware/gateway/build/iot-home-gateway.bin`，`1441920 / 0x160080`，SHA256 `84F88FE37130969A98343F8B6645F5995C98DC29EDC7ADC8583DF4B5630EF9EC`；
- 实验 ON：`firmware/gateway/build-cq/iot-home-gateway.bin`，`1459152 / 0x1643d0`，SHA256 `4BDA8A6E6D391C0E8562094E2C3BDD1E34AF40454DFA36D8415F9416D0950975`。

硬件结果：

- 只烧录 COM6 网关应用区 `0x10000`，未擦 NVRAM，未刷 bootloader/分区表；
- 启动时 I2C 总线初始化成功，但 OLED 地址 `0x3c` probe 返回 `ESP_FAIL`；
- `firmware/gateway/log.cq-clean4h-gateway-20260925-072508.txt` 与 `log.cq-postflash-gateway-20260925-063840.txt` 在重构前已记录同样的“OLED not available”，因此该现象不是本次拆分引入；
- 拆分后执行 5 次 raw APS 查询：5/5 accepted、REPORT、COMPLETE，5 条 confirm 均为 status=0，`ESP_ERR_NO_MEM=0`，无复位；
- C/C++ 复审初版 BLOCK（2 个空指针风险），修复后复审 APPROVE。


## 8. USB Serial/JTAG 传输组件实施记录（2026-09-26）

新增 `firmware/common/iot_usb_serial_jtag`，负责驱动安装、启动、停止以及任务上下文阻塞读写。命令行编辑、`help`、`rf query`、`rf set`、`rf auto` 的解析和 RF 业务动作仍留在应用层。

组件明确区分两类状态：

- 本实例安装驱动：`driver_owned=true`，停止时允许卸载；
- 底层驱动已由外部安装：`driver_owned=false`，停止时只解除本实例状态，不执行卸载。

最终构建结果：

- 默认 OFF：`firmware/gateway/build/iot-home-gateway.bin`，`1441920 / 0x160080`，SHA256 `2B848925087EBB5A241CC6E232D6A43895BA29625E44049C4FBB6CB7F5BBBA67`；
- 实验 ON：`firmware/gateway/build-cq/iot-home-gateway.bin`，`1459552 / 0x164560`，SHA256 `43D8FCD048936077787F244459CAB9F40DE1757BA368FB4DBE97BCD5C324F09720975`。

硬件结果：实验 ON 只写入 COM6 应用区 `0x10000`；稳定在线状态下通过该传输层执行 5 次 raw APS 查询，5/5 accepted、REPORT、COMPLETE，confirm status=0×5，`ESP_ERR_NO_MEM=0`，无复位。C/C++ 复审 **APPROVE**。


## 9. OLED 规格回溯与纵向整帧刷新恢复（2026-09-26）

### 9.1 现象与误判

OLED 排线重新接回后，先后出现整屏上下重复、字体拉长、字迹不清等现象。中途曾根据部分现象怀疑面板为 128x32，并临时把网关装配为 height=32；用户随后观察到画面更像历史 64 帧画面的一角，因此需要回到历史已验证代码做差异对比。

最终经用户确认，当前网关面板应为 **SSD1306 128x64**。128x32 初始化参数仍保留在驱动中，供后续不同面板复用，但该规格没有在本机当前屏幕上完成视觉验证，不能称为本硬件已验证。

### 9.2 历史对比结论

对比历史硬件测试正常版本后确认，当前面板历史上使用：

```text
width=128
height=64
multiplex=0x3f
COM pins=0x12
page window=0..7
framebuffer=1024 bytes
A1 + C8 orientation
```

拆分提交本身保留了上述参数；导致显示异常的关键差异是刷新策略偏离了最早正常方案。最早正常版本使用：

```text
0x20,0x01 vertical addressing
page-major framebuffer -> column-major GRAM conversion
one continuous 1024-byte data transaction per frame
I2C transaction timeout=1000 ms
```

后续横向 addressing、短分块写入方案会让当前面板字迹不清，因此撤回。

### 9.3 最终实现

`iot_ssd1306_config_t` 保留 `width`、`height`，并新增调用方提供的：

- `framebuffer`：应用绘图使用的 page-major 帧缓冲；
- `scratch_buffer`：刷屏前用于转换 GRAM 字节序的临时缓冲；
- 两者大小均不得小于当前面板帧大小，且不应指向同一块存储。

`iot_ssd1306_flush()` 现在执行：

1. 设置 vertical addressing；
2. 设置完整 column/page window；
3. 将 page-major framebuffer 转成 column-major GRAM scratch；
4. 一次连续发送完整帧数据。

驱动和两块缓冲均由 `main.c` 静态提供，驱动内部不做动态分配。

### 9.4 Wi‑Fi 省电策略

网关为电脑 Windows 移动热点场景，将 STA 省电策略从 `WIFI_PS_MAX_MODEM` 调整为 `WIFI_PS_MIN_MODEM`。网关由 USB 供电，优先保证热点兼容性、IP 获取和 MQTT 稳定性；`MIN_MODEM` 仍保留 Wi‑Fi modem sleep，并非完全关闭省电。

### 9.5 验证结果

构建结果：

- 默认 OFF：`firmware/gateway/build/iot-home-gateway.bin`，`1442432 / 0x160280`，SHA256 `50971B5E5F51E5F2F463D153872013CF720C32A851CDC2F23BA799138D3C66D7`；
- 实验 ON：`firmware/gateway/build-cq/iot-home-gateway.bin`，`1460064 / 0x164760`，SHA256 `FDF9D30330CDAAD77F8A36A02634ECB6FC096EB62EBDFBB0679506A1334FFC4C`。

硬件复测：

- 热点开启后的 100 秒 paired 复测中，网关获得 IP `192.168.137.140`，MQTT connected；
- 最终默认固件只写入应用区后，又执行 85 秒复测：节点约 16.7 秒入网，短地址 `0x82cb`；
- 85 秒复测记录到 25 条 RF sample、24 条 cached report、8 条 MQTT telemetry 发布；
- RF power command/confirm 正常，100 秒和 85 秒测试均无崩溃、无异常复位；
- 用户确认 OLED 显示恢复正常。

复审：初次复审 APPROVE（指出 1 个 M、3 个 L 的加固建议）；完成加固后的增量复审 APPROVE，C/H/M/L=0。

## 10. 阶段 C：Wi‑Fi STA 基础连接驱动（2026-09-26）

### 10.1 拆分边界

本阶段新增通用组件 `iot_wifi_sta`，只封装稳定的 ESP-IDF Wi‑Fi STA 底层动作：

- `esp_netif` 初始化、默认事件循环复用和默认 STA netif 创建；
- `esp_wifi_init()` 与 RAM-only Wi‑Fi storage；
- STA 扫描方式、凭据、国家码、省电模式和连接参数；
- 启动 STA；
- 设置默认主动扫描时长和 home channel dwell time。

本阶段**不移动**：

- `wifi_cb()`：仍由应用层处理 Wi‑Fi/IP 事件；
- `wifi_reconnect_task()`：重连和 RF 健康恢复仍保留在 `main.c`；
- provisioning owner gate、recovery quiesce：它们属于服务/策略层，后续单独拆分。

### 10.2 接口形状

组件使用全局 Wi‑Fi 单例，但不隐藏事件注册：

```c
esp_err_t iot_wifi_sta_initialize(void);
esp_err_t iot_wifi_sta_configure(const iot_wifi_sta_config_t *config);
esp_err_t iot_wifi_sta_start(void);
esp_err_t iot_wifi_sta_set_scan_timing(uint16_t active_max_ms,
                                       uint16_t home_chan_dwell_ms);
```

应用层仍在 initialize 之后、start 之前注册 `WIFI_EVENT` 和 `IP_EVENT`，以保证事件回调能继续使用现有 MQTT、Zigbee、OLED 和状态位。

### 10.3 行为约束

- 不引入动态分配；
- 不改变 SSID/password 的来源，仍由产品配置访问器提供；
- 国家码继续固定为 CN，允许 1..13 信道；
- 省电模式通过配置传入，当前网关使用 `WIFI_PS_MIN_MODEM`；
- 凭据继续只存在于 RAM，不写入文档；
- 本阶段完成后必须保持默认 OFF 与实验 ON 均可构建。

### 10.4 实现加固

第一次复审后，在提交前补充了以下防护：

- 使用 `s_initialized / s_configured / s_started` 区分生命周期，避免重复初始化、重复配置或重复启动；
- 记录组件是否拥有全局 netif、默认事件循环和默认 STA netif；初始化失败时按创建顺序回滚；
- `esp_wifi_set_storage(WIFI_STORAGE_RAM)` 失败时中止初始化，不把组件标记为可用；
- 显式校验 SSID 非空且不超过 IDF 数组容量，WPA2 密码长度为 8..63，拒绝 64 字节输入造成静默截断；
- 先设置 STA mode、CN 国家码和省电模式，再把密码放入栈上 `wifi_config_t`；
- `esp_wifi_set_config()` 返回后立即清零栈上凭据结构；
- CMake 只公开 `esp_wifi`，把 `esp_event`、`esp_netif` 和 `log` 作为私有依赖。

### 10.5 验证结果

默认 OFF：

```text
firmware/gateway/build/iot-home-gateway.bin
大小：1443664 / 0x160750
SHA256：D8A55421B6928D0CFACAD3F7313B00BF32FDE112F719298AD2CD287232B7A4C2
```

实验 ON：

```text
firmware/gateway/build-cq/iot-home-gateway.bin
大小：1461280 / 0x164C20
SHA256：B8272A3D93707DCE7208613C9D2EFEBBDF8BD793712BB66A1F6FBB69A0731C96
```

硬件短测：

- COM6 仅写入应用区 `0x10000`，未擦 Zigbee NVRAM，未刷 bootloader/分区表，写入哈希校验通过；
- 日志：`backups/hw-postrefactor-20260926/wifi-sta-refactor-hardened-gateway-75.log`；
- 节点约 20.2 秒入网，短地址 `0x82cb`；
- 采集到 19 条 RF sample、18 条 cached report、6 条 MQTT telemetry 发布；
- RF power confirm 正常；
- 异常复位、Guru Meditation 和 abort 均为 0。

此前冷启动阶段出现过一次 Interrupt Watchdog 重启；重构前日志中已有相同调用栈，最终加固版 75 秒短测未复现，当前不判定为本阶段拆分引入。

### 10.6 最终复审

最终加固版独立复审：**APPROVE**，Critical/High/Medium/Low 均为 0。复审确认生命周期状态、失败回滚、全局 netif/event loop 复用、凭据边界、ESP-IDF API 顺序、运行时行为和 CMake 依赖均无阻塞问题。

### 10.7 后续步骤

下一阶段再评估是否把 Wi‑Fi 事件处理和重连任务迁入 `iot_wifi_connection` 服务。该服务会通过回调或事件接口与 MQTT/Zigbee 解耦，不和本阶段的基础 STA 驱动混在同一次改动中。

## 11. 阶段 D：普通 Wi‑Fi 连接服务（2026-09-26）

### 11.1 拆分边界

在阶段 C 的 `iot_wifi_sta` 基础上，新增 `iot_wifi_connection`：

```text
firmware/common/iot_wifi_connection/
├── CMakeLists.txt
├── include/iot_wifi_connection.h
└── iot_wifi_connection.c
```

该服务接管：

- `WIFI_EVENT` / `IP_EVENT` 事件处理注册；
- STA_START 自动连接；
- STA_DISCONNECTED 后的连接状态更新与任务通知；
- GOT_IP 后的连接状态和事件位更新；
- 指数退避重连；
- 周期性 `esp_wifi_stop()/start()` 恢复；
- `is_up()` 与 `wait_connected()` 接口。

仍保留在应用层：

- provisioning owner gate；
- RF health scan 和目标 SSID 判断；
- 冷启动 boot watchdog；
- deep sleep 射频强复位；
- MQTT、Zigbee、OLED 的状态联动。

### 11.2 回调接口

服务通过回调与应用策略解耦：

- `action_allowed`：按动作询问应用是否允许普通连接恢复；
- `on_disconnected`：应用记录 reason/rssi；
- `on_got_ip`：应用更新 IP、射频环境状态；
- `on_first_connected`：应用清除启动看门狗计数；
- `on_retry`：应用在特定重试次数执行 RF health scan 和 boot watchdog。

这样服务本身不依赖 provisioning、MQTT、Zigbee、OLED 或产品配置组件。

### 11.3 加固点

- 初始化状态防重入；
- event group、任务和事件处理器创建失败时回滚；
- 退避延迟按 64 位计算，避免复用配置较大时溢出；
- 校验任务优先级、任务栈和退避参数；
- 连接状态使用 `volatile`，跨事件任务、连接任务和调用方共享；
- 断开时清除连接事件位；
- 提供 `iot_wifi_connection_deinitialize()`：先注销 IP/Wi‑Fi 事件，再删除连接任务和 event group，并复位回调与连接状态；
- 提供 `iot_wifi_sta_deinitialize()`：必要时 stop/deinit Wi‑Fi，再释放该组件拥有的默认 STA netif、默认事件循环和 netif；
- 应用层 `wifi_start()` 用阶段标记保证失败时按“连接服务 → STA 基础驱动”的顺序逆序清理；
- CMake 把 `esp_wifi`、`esp_event`、`esp_netif`、`freertos` 和 `log` 作为服务私有依赖。

### 11.4 构建结果

默认 OFF：

```text
firmware/gateway/build/iot-home-gateway.bin
大小：1445616 / 0x160ef0
SHA256：6F99D8CFBA0860E72D0CF259E8FA5A35C3249FE5D080CB407F20A7CE2FF45B72
```

实验 ON：

```text
firmware/gateway/build-cq/iot-home-gateway.bin
大小：1463232 / 0x1653c0
SHA256：49A7B1D9407E2FD32543180CCE09B9D05A9D5131B57A3210D7AC78C49EA4FB42
```

### 11.5 硬件短测

最终默认 OFF 固件仅写入 COM6 应用区：

```text
address：0x10000
result：Hash of data verified
Zigbee NVRAM：未擦除
bootloader：未刷写
分区表：未刷写
```

最终提交版本的确认日志：

```text
backups/hw-postrefactor-20260926/wifi-connection-final-confirm-gateway-75.log
```

结果：

```text
取得 IP：1（29.2 秒，192.168.137.140）
MQTT connected：1（31.1 秒）
节点入网：1（约 40.0 秒，短地址 0x82cb）
RF sample：15（14 普通 + 1 RF power report）
cached report：14
MQTT telemetry publish：4
RF power confirm：1
异常复位：0
Guru Meditation：0
abort：0
```

启动前 30 秒热点触发了 3 次 beacon timeout；连接服务自动重新关联并继续完成 IP/MQTT/Zigbee 全链路。日志中有一次 `i2c: clear bus error`，之后采集和刷新继续执行，未见持续显示或总线故障。

在最终边界修正之前，还执行过一次 100 秒干净启动测试：

```text
backups/hw-postrefactor-20260926/wifi-connection-deinit-cleanboot-gateway-100.log
节点 18.6 秒入网，28 条 RF sample，27 条 cached report，9 条 MQTT telemetry，无异常复位。
```

测试期间也出现过一次目标热点可见但关联较慢的情况，随后设备自恢复。结合多次结果，当前判断 Windows 热点与 Zigbee 共存时会出现偶发关联/信标波动；阶段 D 的拆分没有阻断自动恢复。夜间长测继续重点观察该模式是否高频复发。

### 11.6 最终复审

最终加固版独立复审：**APPROVE**，Critical/High/Medium/Low 均为 0。复审确认反初始化顺序、失败路径资源所有权、重复初始化能力、FreeRTOS 优先级边界和正常启动路径均无阻塞问题。

### 11.7 后续方向

下一阶段再评估是否把 RF health scan、boot watchdog 和 deep sleep 进一步抽象为可替换的连接恢复策略；仍应保持小步提交，避免一次性把 provisioning 和正常连接状态机合并。
