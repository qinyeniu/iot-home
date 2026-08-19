"""AHT20 教科书时序：触发一次后只重试读取，不再重复触发。"""

from machine import I2C, Pin
import time

I2C_ADDR = 0x38
i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
print("SCAN:", [hex(a) for a in i2c.scan()])

time.sleep_ms(40)  # 上电稳定

# 初始化
i2c.writeto(I2C_ADDR, b"\xbe\x08\x00")
time.sleep_ms(10)
status = i2c.readfrom(I2C_ADDR, 1)[0]
print("status after init: 0x%02x" % status)

# 触发测量
i2c.writeto(I2C_ADDR, b"\xac\x33\x00")
time.sleep_ms(80)

# 只重试读取，最多 20 次
d = None
for i in range(20):
    try:
        d = i2c.readfrom(I2C_ADDR, 6)
        print("read ok on try %d" % (i + 1))
        break
    except OSError:
        time.sleep_ms(50)

if d is None:
    print("READ NEVER SUCCEEDED")
else:
    hum_raw = (d[1] << 12) | (d[2] << 4) | (d[3] >> 4)
    temp_raw = ((d[3] & 0x0F) << 16) | (d[4] << 8) | d[5]
    hum = hum_raw / 1048576 * 100
    temp = temp_raw / 1048576 * 200 - 50
    print("raw: %s hum: %.1f temp: %.1f" % ([hex(x) for x in d], hum, temp))
