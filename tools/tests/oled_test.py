"""OLED（SSD1306 0.96" I2C）测试：扫描 3 个地址 + 显示温湿度/光照。

驱动内嵌在脚本里（MicroPython 官方 ssd1306.py 的 I2C 版精简实现），
不需要往板子文件系统里装额外文件。
"""

from machine import I2C, Pin
import framebuf
import time

I2C_AHT20 = 0x38
I2C_BH1750 = 0x23
I2C_OLED = 0x3C

i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
print("SCAN:", [hex(a) for a in i2c.scan()])


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
            self.cmd(0x00)  # 列低字节
            self.cmd(0x10)  # 列高字节
            self.data(self.buf[page * self.w:(page + 1) * self.w])


def read_aht20():
    i2c.writeto(I2C_AHT20, b"\xbe\x08\x00")
    time.sleep_ms(10)
    i2c.writeto(I2C_AHT20, b"\xac\x33\x00")
    time.sleep_ms(80)
    d = None
    for _ in range(10):
        try:
            d = i2c.readfrom(I2C_AHT20, 6)
            break
        except OSError:
            time.sleep_ms(50)
    if d is None:
        raise OSError("AHT20 read timeout")
    hum_raw = (d[1] << 12) | (d[2] << 4) | (d[3] >> 4)
    temp_raw = ((d[3] & 0x0F) << 16) | (d[4] << 8) | d[5]
    return round(temp_raw / 1048576 * 200 - 50, 2), round(hum_raw / 1048576 * 100, 1)


def read_bh1750():
    i2c.writeto(I2C_BH1750, b"\x10")
    time.sleep_ms(180)
    d = i2c.readfrom(I2C_BH1750, 2)
    return round(((d[0] << 8) | d[1]) / 1.2, 1)


if I2C_OLED not in i2c.scan():
    print("OLED NOT FOUND on bus!")
else:
    oled = SSD1306_I2C(i2c, addr=I2C_OLED)
    oled.fb.fill(0)
    oled.fb.text("IOT-HOME", 24, 0)
    oled.fb.text("T:--.-C", 0, 16)
    oled.fb.text("H:--%", 0, 32)
    oled.fb.text("L:-- lx", 0, 48)
    oled.show()
    print("OLED initialized, showing placeholder")

    for r in range(3):
        try:
            t, h = read_aht20()
            oled.fb.fill_rect(0, 16, 128, 32, 0)
            oled.fb.text(f"T:{t}C", 0, 16)
            oled.fb.text(f"H:{h}%", 0, 32)
        except OSError as e:
            print(f"ROUND {r}: AHT20 FAIL ({e})")
        try:
            lux = read_bh1750()
            oled.fb.fill_rect(0, 48, 128, 8, 0)
            oled.fb.text(f"L:{lux} lx", 0, 48)
        except OSError as e:
            print(f"ROUND {r}: BH1750 FAIL ({e})")
        oled.show()
        print(f"ROUND {r}: OLED updated")
        time.sleep_ms(1500)
