"""BH1750（GY-302）光照读取（MicroPython 版，跑在 ESP32-C6 上）。

用法同 AHT20：触发测量 -> 等 180ms -> 读 2 字节。
注意：BH1750 没有寄存器地址，直接写命令、然后读数据。
"""

from machine import I2C, Pin
import time

I2C_ADDR = 0x23  # ADDR 接地时默认地址

i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
print("SCAN:", [hex(a) for a in i2c.scan()])


def read_lux():
    # 0x10 = H 分辨率模式（1 lx 精度，最大量程 65535 lx）
    i2c.writeto(I2C_ADDR, b"\x10")
    time.sleep_ms(180)  # H 模式测量最长约 180ms
    d = i2c.readfrom(I2C_ADDR, 2)
    raw = (d[0] << 8) | d[1]
    # 官方公式：光照度 = 原始值 / 1.2
    return raw / 1.2


for attempt in range(1, 6):
    try:
        lux = read_lux()
        print(f"ATTEMPT {attempt}: LUX: {lux:.1f}")
        break
    except OSError as e:
        print(f"ATTEMPT {attempt}: failed ({e})")
        time.sleep_ms(200)
