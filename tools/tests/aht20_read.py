"""AHT20 温湿度读取（MicroPython 版，跑在 ESP32-C6 上）。

要点：
- 触发测量后不能立刻读，要等传感器“忙”位（bit7）清零；
- 初始化后要确认“已校准”位（bit3=1），否则读出来是 0 / -50C；
- 读不到就重试几次。
"""

from machine import I2C, Pin
import time

I2C_ADDR = 0x38

i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
print("SCAN:", [hex(a) for a in i2c.scan()])


def read_status():
    """读 1 字节状态。读不到时返回 0x80，当作“还在忙”。"""
    try:
        return i2c.readfrom(I2C_ADDR, 1)[0]
    except OSError:
        return 0x80


def read_aht20():
    # 初始化（数据手册：0xBE 0x08 0x00）
    i2c.writeto(I2C_ADDR, b"\xbe\x08\x00")
    time.sleep_ms(10)

    # 触发测量，等 80ms 后直接读 6 字节（实测：不要反复读状态，否则数据为 0）
    i2c.writeto(I2C_ADDR, b"\xac\x33\x00")
    time.sleep_ms(80)

    # 读取失败就稍等重试（不重复触发）
    d = None
    for _ in range(10):
        try:
            d = i2c.readfrom(I2C_ADDR, 6)
            break
        except OSError:
            time.sleep_ms(50)
    if d is None:
        raise OSError("AHT20 read timeout")

    hum_raw = (d[1] << 12) | (d[2] << 4) | (d[3] >> 4)
    temp_raw = ((d[3] & 0x0F) << 16) | (d[4] << 8) | d[5]
    hum = round(hum_raw / 1048576 * 100, 1)
    temp = round(temp_raw / 1048576 * 200 - 50, 2)
    return temp, hum


for attempt in range(1, 6):
    try:
        temp, hum = read_aht20()
        print(f"ATTEMPT {attempt}: HUM: {hum}%  TEMP: {temp}C")
        break
    except OSError as e:
        print(f"ATTEMPT {attempt}: failed ({e})")
        time.sleep_ms(200)
