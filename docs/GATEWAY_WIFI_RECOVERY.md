# 网关 WiFi 反复 NO / reason 201/36 排查与自愈记录

**更新时间**：2026-09-10  
**适用设备**：ESP32-C6 网关（COM6），2.4GHz 家用路由器

## 1. 典型现象

OLED 显示：

```text
WIFI:NO MQTT:NO
IP:0.0.0.0
UP: 秒数持续增加
```

串口可能看到：

```text
WiFi disconnected: reason=201 rssi=-128
WiFi disconnected: reason=36 rssi=-128
```

含义：

| reason | 含义 | 判断 |
|---|---|---|
| 201 | `NO_AP_FOUND` | 扫描不到目标 SSID，常见于 2.4G 射频关闭/改名/隐藏/信道变化 |
| 36 | `ASSOC_EXPIRE` | 找到了或尝试关联 AP，但收不到 AP 响应，关联超时 |
| `rssi=-128` | 没有有效信号 | 不是密码错误 |

如果是密码错误，通常会出现认证失败类 reason，而不是 201/36。

## 2. 先区分：路由器问题还是板子问题

### 路由器侧检查

ESP32-C6 只能连接 2.4GHz，不能连接 `CU_0D84_5G` 这类 5GHz SSID。

在电脑上扫描：

```powershell
netsh wlan show networks mode=bssid
```

如果看不到 2.4GHz `CU_0D84`，只看到 5GHz 热点，优先：

1. 重启路由器，等待 1–2 分钟；
2. 进入路由器管理页确认 2.4GHz 已启用、未隐藏；
3. 确认 SSID 与本机 `firmware/gateway/main/wifi_secrets.h` 一致；
4. 信道先固定 1/6/11，频宽 20MHz，WPA2-PSK/AES；
5. 检查 MAC 接入控制/黑名单。

### 板子侧判据

- OLED 正常但 `WIFI:NO`：显示链路没问题，问题在 WiFi/AP；
- 有 IP 后 MQTT 才连接；没有 IP 时的 `Host is unreachable` 是结果，不是根因；
- Zigbee 可在 WiFi 断联时独立启动，不能用 Zigbee 正常推断 WiFi 正常。

## 3. 固件已经实现的本地自愈

位置：`firmware/gateway/main/main.c`

1. **全信道扫描**：`WIFI_ALL_CHANNEL_SCAN`，不依赖旧信道缓存。
2. **WiFi 配置只放 RAM**：`WIFI_STORAGE_RAM`，避免路由器从历史信道迁移后沿用旧 BSSID/信道。
3. **底层原因日志**：断线事件打印 reason 与 RSSI。
4. **启动看门狗**：启动 25 秒仍没拿到 IP，则 `esp_restart()`，最多 2 次。
5. **RTC 保持计数 + 魔数校验**：
   - 软件热重启/RST 保留计数，防止无限重启；
   - 真正重新断电后计数自动重新从 0 开始；
   - 成功拿到 IP 后清零。
6. **长时退避重连**：看门狗次数用完后继续指数退避，并周期性 stop/start WiFi。

> 旧实现把重启计数写入 NVS。若两次热重启时 AP 恰好不可用，计数会长期保持为 2，之后即使重新上电也不再自愈。2026-09-10 已改为 RTC 计数。

## 4. 实机记录（2026-09-10）

- 修复 OLED 后，曾出现 WiFi NO。
- 电脑扫描只看到 5GHz `CU_0D84_5G`，看不到 2.4GHz `CU_0D84`。
- 网关串口重复报 `reason=201 rssi=-128` 与 `reason=36 rssi=-128`。
- 某一轮 2.4G 短暂恢复时，看门狗第 1 次热重启后成功拿到 `192.168.10.112`，并显示 `MQTT connected`。
- 随后 2.4G 再次不可见，连续两次热重启仍是 201/36。该结果说明：固件自愈只能在 AP 恢复后生效，不能替代重启/修复路由器 2.4G 射频。

## 5. 验收标准

以下全部满足才算网关网络恢复：

1. OLED 显示 `WIFI:OK MQTT:OK` 和非 `0.0.0.0` IP；
2. 串口出现 `WiFi OK, IP: ...` 与 `MQTT connected`；
3. MQTT 收到 `iot-home/gw-001/nodes/gw-001/status` 心跳；
4. 连续观察 10 分钟无断联；
5. 再继续 COM5 终端烧录和 Zigbee 端到端联调。
