# 2026-09-20 夜间交接：角色互换最小复现 → 节点 COM5 接收链路疑似硬件退化

> 本文件是今晚（约 23:30–24:00）接续丢失对话的调查记录。
> 白天上下文先看 `docs/HANDOFF_2026-09-20.md`（10:40 版）。

## 1. 新对话先看这里

1. 上午 10:40 交接之后，今天还有一段对话（约 10:41–23:00），其上下文已丢失；它做了 6 个提交并把大量改动留在工作区。
2. 今晚已确认：双端当前**没有**运行生产固件，而是运行一次性角色互换诊断固件（20 dBm 版本，23:37 刷入）。
3. 诊断结论（有实测证据）：**节点 COM5 的接收链路当前严重退化或被去敏**——它发出的关联请求协调器能收到，但它收不到协调器的关联响应；信标 LQI 只有 5–15。今早同样的摆位链路完全正常。
4. 这是物理层问题（硬件天线/去敏/摆位距离），不是 Zigbee 应用逻辑问题。下一步必须先做物理检查，不要继续改协议代码。
5. 未 push、未部署服务器、未擦生产 NVS；今晚刷写只写了应用分区。
6. 不要 `git add -A`；工作区已暂存/未暂存的改动保持原样等用户决定（见第 6 节）。

## 2. 丢失对话（今天白天）的提交记录

分支 `codex/gateway-oled-wifi-checkpoint`，在 1b1d20b 之后又有 6 个提交：

```text
2b4eb22 16:44 fix(node): verify provisional rejoins with sensor reports
1fb87d6 15:47 fix(node): extend provisional rejoin APS verification
3180923 15:23 fix(node): verify provisional Zigbee rejoins
3c166c2 14:39 fix(gateway): isolate Wi-Fi power-save coexistence test
de86d94 14:10 fix(gateway): prioritize Zigbee during Wi-Fi coexistence
76e7b5c 10:41 docs(handoff): record stable node rejoin recovery
```

此外 `firmware/gateway/main/main.c` 与 `firmware/node_zigbee/main/main.c` 当前都有
**已暂存 + 未暂存**两层改动（git status 显示 MM），是丢失对话进行中的工作，今晚没有动它们。

## 3. 丢失对话的工作目录（关键）

丢失对话把所有实验材料放在仓库外：

```text
C:\Users\HJB\.iot-home\
├── captures\                         # 每次测试的双端串口日志（18:27–23:37 共 40+ 组）
├── gateway-rebuild-backup-20260920-194740
├── role-swap-backup-20260920-204015
├── role-swap-temp-state-before-original-direction-20260920-210146
│       └── 两块板子的 zb_storage(0x180000,16KB) 与 zb_fct(0x184000,4KB) 备份
├── role-swap-diag-20260920-2045  /2052 /2053   # 反向：coord COM5 / join COM6
├── role-swap-diag-20260920-212525-original     # 正向：coord COM6 / join COM5
├── role-swap-diag-20260920-2220-reverse-same-logic
├── role-swap-diag-20260920-2240-lowtx-original # 正向 + 发射功率 -15 dBm
├── role-swap-diag-20260920-2330-maxtx-original # 今晚新建：正向 + 发射功率 20 dBm
├── capture_roleswap_*.py                       # 双端/单端同步抓包脚本
└── wifi_secrets_home_20260911.h / wifi_secrets_hotspot_20260911.h
```

## 4. 角色互换实验设计与结果

目的：用最干净的一次性固件（无 Wi‑Fi、无传感器、无业务逻辑）隔离
"节点无法回联/关联"问题，判断它是协议逻辑问题还是射频问题。

- coord-on-com6：临时协调器，channel 26，开机即组网并开放入网 255 秒；
  `esp_zb_nvram_erase_at_start(true)` → 每次启动都是全新 PAN。
- join-on-com5：临时 ED（rx-on-when-idle），扫描并加入。

时间线（实测）：

| 时间 | 配置 | 结果 |
|---|---|---|
| 21:32 | 正向，默认功率 | **成功** JOINED，PAN 0xda11，short 0xf75f（耗时 ~68 秒） |
| 21:42–22:28 | oneshot / router / ch25 / ch26 无限重试 / 反向 | 全部失败，扫描不到或无法完成关联 |
| 22:33–22:37 | 外部协调器，只复位 ED（-15 dBm） | 失败；`low lqi 5–15`，`Can't find PAN` |
| 22:43 | -15 dBm 双端刷入 | 持续失败 40+ 分钟 |
| 23:37–23:41 | **20 dBm 满功率**双端（今晚的对照实验） | 仍失败，220 秒无法加入 |

### 23:37 对照实验的关键证据

节点 COM5（ED）侧：

```text
W (57877) ... bdb_status=3 ... pan=0x39a7 channel=26      # 识别到了协调器
ZBOSS: nwk/nwk_join.c:293  Will assoc to pan 0x39a7 on channel 26
ZBOSS: mac/mac_data.c:835  polling during association error: 233
ZBOSS: nwk/nwk_join.c:251  No dev for join                 # 关联响应始终收不到
```

协调器 COM6 侧：

```text
nwk/nwk_cr_join.c:1285 association: status 0, address 0xd109,
                   device 98:88:e0:ff:fe:71:1b:a0          # 上行：收到关联请求
nwk_cr_join.c:1285 association: status 0, address 0xe5f9   # 重试也收到
```

结论：

1. **上行通**（节点发 → 协调器收，status 0，两次）。
2. **下行不通**（协调器发关联响应 → 节点收不到）；节点测到信标 `low lqi 5–15`。
3. 功率恢复 20 dBm 后依旧如此，排除"只是 -15 dBm 太远"的单因素。
4. 今早 10:35 同样摆位/同角色链路正常并持续入库 → **退化发生在今天白天**。
5. 软件刷写不会损伤接收，因此指向：节点天线/匹配件物理损伤、天线区被金属遮挡、
   USB 线缆/端口噪声去敏，或者两块板子当前实际距离很远（需用户确认摆位）。

## 5. 下次继续的建议顺序（需要用户在硬件旁）

1. 先向用户确认物理事实：
   - 两块板子现在是否还紧挨着？节点白天有没有被挪动/带走/跌落？
   - 节点的 USB 线/插口今天有没有换？天线区域有没有压到金属？
2. 物理检查：把两块板子并排、天线区域无遮挡，换一根 USB 线、换一个 USB 口（直连，不走 hub）。
3. 重跑 23:30 正向满功率测试：
   `C:\Users\HJB\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe
   C:\Users\HJB\.iot-home\capture_roleswap_original.py`
   预期成功标志：节点侧 `ROLESWAP_JOINED`，协调器侧 `DEVICE_ANNCE`。
4. 若并排、换线后节点仍 `low lqi`，基本可判定节点模块接收前端硬件故障：
   - 检查模块是 PCB 天线还是 IPEX（-1U）版本及 0Ω 选择电阻位置；
   - 需要考虑返修/换板。
5. 射频链路确认正常后，再决定生产固件的刷回方案：
   - 网关：`firmware/gateway/build/iot-home-gateway.bin`（20:13 构建，含 PAN 0x2b9e 重建逻辑、MAX_MODEM）；
   - 节点：`firmware/node_zigbee/build/iot-home-zigbee-node.bin`（19:03 构建）；
   - 21:01 的 NVRAM 备份可用于恢复实验前 Zigbee 状态。
6. 最后再处理工作区 MM 改动的取舍与提交（疑似"先回退 provisional rejoin，再演进"的半成品）。

## 6. 当前设备与端口

| 角色 | 端口 | BASE MAC | 当前固件 |
|---|---:|---|---|
| 节点 | COM5 | 98:88:E0:71:1B:A0 | role_swap_join（20 dBm，23:37 刷），关联不上 |
| 网关 | COM6 | 98:88:E0:70:90:DC | role_swap_coord（20 dBm，23:37 刷），已组网 PAN 0x39a7 |

云端（只读，23:20 查询）：双端 offline；网关 last_seen 20:57；
节点 last 遥测 16:03（温度 32.2 / 湿度 67.6 / 光照 55）。

## 7. 待清理的杂项（未执行）

- `firmware/node_zigbee/460800/`：丢失对话误建的构建目录（把波特率当成了 -B 目录），
  只有 configure 没编译完；未跟踪垃圾，删除被本机策略拦了，待用户确认后删。
- `tools/capture_serial.py`：今晚为抓单端日志新建的小工具，未跟踪。

## 8. 边界（继续遵守）

- 不读、不打印 wifi_secrets.h、mqtt_secrets.h；密码不落文档/日志/Git。
- 不 push，不部署服务器；不擦生产 Flash / zb NVRAM。
- 不把未跟踪文件混入提交；不用 `git add -A`。
