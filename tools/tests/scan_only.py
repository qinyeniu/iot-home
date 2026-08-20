"""纯 I2C 扫描（MicroPython 版）：列出总线上所有设备地址。"""

from machine import I2C, Pin

i2c = I2C(0, sda=Pin(2), scl=Pin(3), freq=100000)
devs = i2c.scan()
print("SCAN:", [hex(a) for a in devs])
print("EXPECT: 0x38 AHT20 | 0x23 BH1750 | 0x3C OLED")
