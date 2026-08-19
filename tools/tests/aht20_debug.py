"""AHT20 时序调试：用多种等待时长尝试读取，找出模块吃哪套节奏。"""

from machine import I2C, Pin
import time

I2C_ADDR = 0x38
i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
print("SCAN:", [hex(a) for a in i2c.scan()])

# 上电后先等 100ms，再初始化
time.sleep_ms(100)
try:
    i2c.writeto(I2C_ADDR, b"\xba")  # 软复位
except OSError:
    pass
time.sleep_ms(100)
i2c.writeto(I2C_ADDR, b"\xbe\x08\x00")
time.sleep_ms(100)

status = i2c.readfrom(I2C_ADDR, 1)[0]
print("status after init: 0x%02x" % status)

for delay in (60, 80, 100, 150, 200, 300):
    try:
        i2c.writeto(I2C_ADDR, b"\xac\x33\x00")
        time.sleep_ms(delay)
        d = i2c.readfrom(I2C_ADDR, 6)
        hum_raw = (d[1] << 12) | (d[2] << 4) | (d[3] >> 4)
        temp_raw = ((d[3] & 0x0F) << 16) | (d[4] << 8) | d[5]
        hum = hum_raw / 1048576 * 100
        temp = temp_raw / 1048576 * 200 - 50
        print("delay %3d ms -> raw %s hum %.1f temp %.1f" % (delay, [hex(x) for x in d], hum, temp))
        time.sleep_ms(100)
    except OSError as e:
        print("delay %3d ms -> OSError %s" % (delay, e))
        time.sleep_ms(100)
