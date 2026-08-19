"""双传感器联合测试：AHT20 温湿度 + BH1750 光照（MicroPython 版）。

连读 3 轮，每轮输出两个传感器的数据；某个传感器失败不影响另一个。
"""

from machine import I2C, Pin
import time

I2C_AHT20 = 0x38
I2C_BH1750 = 0x23

i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
print("SCAN:", [hex(a) for a in i2c.scan()])


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
    temp = round(temp_raw / 1048576 * 200 - 50, 2)
    hum = round(hum_raw / 1048576 * 100, 1)
    return temp, hum


def read_bh1750():
    i2c.writeto(I2C_BH1750, b"\x10")
    time.sleep_ms(180)
    d = i2c.readfrom(I2C_BH1750, 2)
    raw = (d[0] << 8) | d[1]
    return round(raw / 1.2, 1)


for r in range(1, 4):
    parts = []
    try:
        t, h = read_aht20()
        parts.append(f"AHT20: {t}C / {h}%")
    except OSError as e:
        parts.append(f"AHT20: FAIL ({e})")

    try:
        lux = read_bh1750()
        parts.append(f"BH1750: {lux} lx")
    except OSError as e:
        parts.append(f"BH1750: FAIL ({e})")

    print(f"ROUND {r}: " + " | ".join(parts))
    time.sleep_ms(300)
