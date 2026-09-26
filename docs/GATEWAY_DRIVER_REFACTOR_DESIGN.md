# 网关驱动拆分与基础驱动类型设计

日期：2026-09-26
适用固件：`firmware/gateway`
状态：阶段 A/B 已完成。基础驱动类型、可复用 SSD1306 组件和 USB Serial/JTAG 传输组件已落地；默认/实验构建通过，拆分后硬件短测 PASS。当前面板经用户确认按 128x64、vertical addressing 整帧刷新，显示正常。

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
