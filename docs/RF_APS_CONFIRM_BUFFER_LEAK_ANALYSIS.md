# 网关 raw APS 发送 buffer 泄漏分析与最小修复

更新时间：2026-09-26（Asia/Shanghai）

## 1. 问题现象

两次 4 小时长测均出现相同模式：

- 2026-09-26：前 32 次 raw APS RF QUERY 返回 `ESP_OK`，第 33 次开始返回 `ESP_ERR_NO_MEM`，之后 5 分钟、10 分钟、30 分钟退避重试仍全部失败。
- 2026-09-25 clean 4h：前 33 次成功，之后开始 `ESP_ERR_NO_MEM`，退避节奏与 2026-09-26 一致。
- 失败期间节点标准 Zigbee 遥测仍持续到达，说明网关 RX/缓存路径仍工作；问题集中在 raw APS 发送 buffer 的获取。

## 2. 已确认的调用链

### 2.1 应用发送路径

网关在：

```text
firmware/gateway/main/main.c
```

中调用：

```c
esp_zb_aps_data_request(&req)
```

ESP-Zigbee wrapper 的静态反汇编显示：

1. 通过 `zb_buf_get_func()` 获取 raw APS buffer；
2. 获取失败时返回 `257`，对应 `ESP_ERR_NO_MEM`；
3. 成功时初始化 APS request，并调用 `zb_buf_flags_or_func(..., 128)`；
4. 通过 `zb_schedule_callback(zb_apsde_data_request)` 提交给 ZBOSS。

其中第 3 步给 buffer 打上的 `128` 标记表示该发送 buffer 的 confirm 由 user-payload 回调路径负责。

### 2.2 APS ACK 后的路径

`zb_apsde_data_acknowledged()` 在成功收到 APS ACK 后：

1. 读取 buffer flag `128`；
2. 调用 `zb_aps_call_user_payload_cb()`；
3. 该函数清掉 `128` 标记，并读取 `g_zb + 0x11f8` 中的 user-data TX callback；
4. 若回调为空，函数返回“已处理”；上层不会释放该 raw APS buffer；
5. 若回调存在，回调执行后上层同样不直接释放，而由已安装的内部 confirm wrapper 释放。

## 3. 根因

节点端注册了 APS confirm：

```c
esp_zb_aps_data_confirm_handler_register(aps_data_confirm_cb);
```

网关端此前只注册了 raw APS indication，没有注册 APS confirm。

`esp_zb_aps_data_confirm_handler_register()` 的实现会先把内部 `zb_aps_data_confirm_handler()` 安装为 ZBOSS user-data TX callback。该内部 wrapper 在应用 confirm 返回后负责释放原始 ZBOSS buffer。

网关未调用此注册函数时：

- `g_zb + 0x11f8` 回调保持空；
- raw APS wrapper 已设置 `128` ownership 标记；
- APS ACK 到达后没有对象完成释放；
- 每次 raw APS 成功发送都会占用一个 buffer；
- 当可用 buffer 被耗尽后，后续 `esp_zb_aps_data_request()` 在入口分配阶段返回 `ESP_ERR_NO_MEM`。

因此这是 raw APS confirm ownership 缺失导致的持久性 buffer 泄漏。外部 Wi‑Fi/MQTT 异常可能影响时序，但不是根因；单纯增大 buffer pool 或增加退避重试只会推迟失败。

## 4. 最小修复

在网关初始化阶段注册一个短 APS confirm callback：

- callback 只记录状态和目标信息；
- 不直接在 callback 中释放 `confirm.asdu`；
- 不从中继续发送 Zigbee 帧；
- 注册动作同时安装 ZBOSS 内部 wrapper，由该 wrapper 在 callback 返回后释放原始 buffer。

## 5. 验证计划

1. 构建默认 OFF 网关，确认未破坏现网路径；
2. 构建实验 ON 网关，确认注册代码和 raw APS 路径通过编译；
3. 先做硬件短测，确认发送后可见 APS confirm，且节点 REPORT 关联仍正常；
4. 后续再安排缩短周期或长测，要求成功发送数显著超过此前 32–33 次阈值且不再出现 `ESP_ERR_NO_MEM`；
5. 不擦 Zigbee NVRAM，不刷 bootloader/分区表，不 push。
