"""环境监测终端整合演示程序 v2（MicroPython，ESP32-C6）。

功能：
- 每 2 秒读取 AHT20（温湿度）+ BH1750（光照）；
- OLED（SSD1306, 0x3C）显示 4 行数据；
- 按键（IO14）按一下翻转继电器（IO20）；
- 板载融合：WS2812（IO8）暗色状态灯、C 口旁蓝灯（IO15）遥测活动灯、
  BOOT 键（IO9）长按 1.5 秒开关指示灯；
- 串口打印 JSON 遥测（未来直接对接 MQTT 格式）。

运行方式：复制为板内 main.py 上电自启；停止：Ctrl+C。
引脚：SDA=IO2 / SCL=IO3 / 按键=IO14 / 继电器=IO20 / WS2812=IO8 / 蓝灯=IO15 / BOOT=IO9
"""

from machine import I2C, Pin
import framebuf
import json
import neopixel
import time

NODE_ID = "sensor-1"

i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
BTN = Pin(14, Pin.IN, Pin.PULL_UP)
RELAY = Pin(20, Pin.OUT)
RELAY.value(1)  # 初始：继电器断开

# ---------- 继电器保护参数 ----------
BTN_DEBOUNCE_MS = 30        # 按键消抖：状态稳定 30ms 才算一次按下
RELAY_MIN_INTERVAL_MS = 800  # 继电器最小切换间隔（防 220V 下快速通断烧器件）

# ---------- 板载资源 ----------
NP = neopixel.NeoPixel(Pin(8), 1)  # WS2812 RGB
ACT_LED = Pin(15, Pin.OUT)  # C 口旁蓝色状态灯
BOOT = Pin(9, Pin.IN, Pin.PULL_UP)  # BOOT 键
indicator_on = True


def set_indicator(rgb):
    """WS2812 状态灯（受指示灯总开关控制，始终用暗色）。"""
    NP[0] = rgb if indicator_on else (0, 0, 0)
    NP.write()


def act_blink():
    """遥测活动灯：C 口旁蓝色灯闪一下。"""
    ACT_LED.value(1)
    time.sleep_ms(60)
    ACT_LED.value(0)


# ---------- AHT20 ----------
def read_aht20():
    i2c.writeto(0x38, b"\xbe\x08\x00")
    time.sleep_ms(10)
    i2c.writeto(0x38, b"\xac\x33\x00")
    time.sleep_ms(80)
    d = None
    for _ in range(10):
        try:
            d = i2c.readfrom(0x38, 6)
            break
        except OSError:
            time.sleep_ms(50)
    if d is None:
        raise OSError("AHT20 read timeout")
    hum_raw = (d[1] << 12) | (d[2] << 4) | (d[3] >> 4)
    temp_raw = ((d[3] & 0x0F) << 16) | (d[4] << 8) | d[5]
    return round(temp_raw / 1048576 * 200 - 50, 2), round(hum_raw / 1048576 * 100, 1)


# ---------- BH1750 ----------
def read_bh1750():
    i2c.writeto(0x23, b"\x10")
    time.sleep_ms(180)
    d = i2c.readfrom(0x23, 2)
    return round(((d[0] << 8) | d[1]) / 1.2, 1)


# ---------- OLED（SSD1306）----------
class SSD1306_I2C:
    def __init__(self, i2c, addr=0x3C, w=128, h=64):
        self.i2c = i2c
        self.addr = addr
        self.w = w
        self.h = h
        self.buf = bytearray(w * ((h + 7) // 8))
        self.fb = framebuf.FrameBuffer(self.buf, w, h, framebuf.MONO_VLSB)
        self.init_display()

    def cmd(self, c):
        self.i2c.writeto(self.addr, b"\x00" + bytes([c]))

    def data(self, chunk):
        self.i2c.writeto(self.addr, b"\x40" + chunk)

    def init_display(self):
        for c in (
            0xAE, 0x20, 0x00, 0xC8, 0x40, 0x81, 0x7F, 0xA1, 0xA6, 0xA8, 0x3F,
            0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x40,
            0x8D, 0x14, 0xAF,
        ):
            self.cmd(c)

    def show(self):
        for page in range(self.h // 8):
            self.cmd(0xB0 + page)
            self.cmd(0x00)
            self.cmd(0x10)
            self.data(self.buf[page * self.w:(page + 1) * self.w])


oled = SSD1306_I2C(i2c)
oled.fb.fill(0)
oled.fb.text("IOT-HOME", 24, 0)
oled.fb.text("T:--.-C", 0, 16)
oled.fb.text("H:--%", 0, 32)
oled.fb.text("RELAY:OFF", 0, 48)
oled.show()

# 开机自检：暗绿闪一下后熄灭
set_indicator((0, 6, 0))
time.sleep_ms(400)
set_indicator((0, 0, 0))


relay_state = 0

# ---------- 主循环 ----------
start_ms = time.ticks_ms()
last_tele = time.ticks_ms()
boot_held_since = None
boot_long_done = False
btn_last = BTN.value()
btn_stable_since = time.ticks_ms()
btn_armed = True
last_relay_change = 0
print("DEMO START", NODE_ID)
while True:
    now = time.ticks_ms()

    # 按键（IO14）：消抖 + 单次触发 + 继电器最小间隔保护
    v = BTN.value()
    if v != btn_last:
        btn_stable_since = now
        btn_last = v
    if (
        v == 0
        and btn_armed
        and time.ticks_diff(now, btn_stable_since) >= BTN_DEBOUNCE_MS
        and time.ticks_diff(now, last_relay_change) >= RELAY_MIN_INTERVAL_MS
    ):
        relay_state = 1 - relay_state
        RELAY.value(0 if relay_state else 1)  # 低电平触发：ON=低电平，OFF=高电平
        last_relay_change = now
        btn_armed = False  # 松手前不再触发
        print(json.dumps({"event": "button", "relay": relay_state}))
    elif v == 1:
        btn_armed = True

    # BOOT 键长按检测：按住 1.5 秒 = 开关指示灯
    if BOOT.value() == 0:
        if boot_held_since is None:
            boot_held_since = now
        elif not boot_long_done and time.ticks_diff(now, boot_held_since) >= 1500:
            indicator_on = not indicator_on
            boot_long_done = True
            # 反馈闪灯：开=暗绿，关=暗红，然后恢复正常状态灯
            NP[0] = (0, 8, 0) if indicator_on else (10, 0, 0)
            NP.write()
            time.sleep_ms(300)
            set_indicator((0, 0, 6) if relay_state else (0, 0, 0))
            print(json.dumps({"event": "indicator", "on": indicator_on}))
    else:
        boot_held_since = None
        boot_long_done = False

    # 每 2 秒遥测一次
    if time.ticks_diff(now, last_tele) >= 2000:
        last_tele = now
        try:
            temp, hum = read_aht20()
            lux = read_bh1750()

            oled.fb.fill_rect(0, 16, 128, 40, 0)
            oled.fb.text("T:%.1fC" % temp, 0, 16)
            oled.fb.text("H:%.1f%%" % hum, 0, 32)
            oled.fb.text("LUX:%.1f" % lux, 0, 48)
            oled.fb.text("RELAY:%s" % ("ON" if relay_state else "OFF"), 64, 48)
            oled.show()

            uptime = round(time.ticks_diff(time.ticks_ms(), start_ms) / 1000, 1)
            payload = {
                "node": NODE_ID,
                "temp": temp,
                "hum": hum,
                "lux": lux,
                "relay": relay_state,
                "indicator": indicator_on,
                "uptime": uptime,
            }
            print(json.dumps(payload))

            act_blink()
            # 继电器 ON = 暗蓝常亮；OFF = 熄灭
            set_indicator((0, 0, 6) if relay_state else (0, 0, 0))
        except OSError as e:
            print("SENSOR ERR:", e)
            # 传感器错误：暗红闪 3 下后恢复（避免误导常亮）
            for _ in range(3):
                set_indicator((8, 0, 0))
                time.sleep_ms(150)
                set_indicator((0, 0, 0))
                time.sleep_ms(150)

    time.sleep_ms(50)
