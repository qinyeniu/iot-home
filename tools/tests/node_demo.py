"""环境监测终端整合演示程序（MicroPython，ESP32-C6）。

功能：
- 每 2 秒读取 AHT20（温湿度）+ BH1750（光照）；
- OLED（SSD1306, 0x3C）显示 4 行数据；
- 按键（IO14）按一下翻转继电器（IO20）；
- 串口打印 JSON 遥测（未来直接对接 MQTT 格式）。

运行方式：串口助手粘贴运行；或复制为板内 main.py 上电自启。
停止：Ctrl+C。

引脚：SDA=IO2 / SCL=IO3 / 按键=IO14 / 继电器=IO20
"""

from machine import I2C, Pin
import framebuf
import json
import time

NODE_ID = "sensor-1"

i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
BTN = Pin(14, Pin.IN, Pin.PULL_UP)
RELAY = Pin(20, Pin.OUT)
RELAY.value(1)  # 初始：继电器断开


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


relay_state = 0


def on_button(pin):
    """按键中断：下降沿（按下）时翻转继电器。"""
    global relay_state
    time.sleep_ms(30)  # 简单去抖
    if BTN.value() == 0:
        relay_state = 1 - relay_state
        RELAY.value(relay_state)
        print(json.dumps({"event": "button", "relay": relay_state}))


BTN.irq(trigger=Pin.IRQ_FALLING, handler=on_button)

# ---------- 主循环 ----------
start_ms = time.ticks_ms()
print("DEMO START", NODE_ID)
while True:
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
            "uptime": uptime,
        }
        print(json.dumps(payload))
    except OSError as e:
        print("SENSOR ERR:", e)

    time.sleep_ms(2000)
