"""板载外设探测 v2：用继电器咔哒声提醒观察时机。

提示音含义：
- 1 声咔哒 = 看 WS2812（暗红→暗绿→暗蓝→灭）
- 2 声咔哒 = 看 IO15 是否有灯跟着闪
- 3 声咔哒 = 立刻按住 BOOT 键（IO9）3 秒
"""

from machine import Pin
import neopixel
import time

RELAY = Pin(20, Pin.OUT)
RELAY.value(1)  # 断开


def click(n=1):
    for _ in range(n):
        RELAY.value(0)
        time.sleep_ms(250)
        RELAY.value(1)
        time.sleep_ms(250)


np = neopixel.NeoPixel(Pin(8), 1)

# 阶段1：1 声咔哒 -> 看 WS2812
click(1)
time.sleep_ms(600)
np[0] = (4, 0, 0)
np.write()
print("WS2812 RED (2.5s)")
time.sleep_ms(2500)
np[0] = (0, 4, 0)
np.write()
print("WS2812 GREEN (2.5s)")
time.sleep_ms(2500)
np[0] = (0, 0, 4)
np.write()
print("WS2812 BLUE (2.5s)")
time.sleep_ms(2500)
np[0] = (0, 0, 0)
np.write()
print("WS2812 OFF (2s)")
time.sleep_ms(2000)

# 阶段2：2 声咔哒 -> 看 IO15
click(2)
time.sleep_ms(600)
led15 = Pin(15, Pin.OUT)
for i in range(8):
    led15.value(i % 2)
    print("IO15 =", i % 2)
    time.sleep_ms(300)
led15.value(0)

# 阶段3：3 声咔哒 -> 按 BOOT 键
click(3)
print("NOW PRESS BOOT (5s window)")
boot = Pin(9, Pin.IN, Pin.PULL_UP)
t0 = time.ticks_ms()
last = boot.value()
while time.ticks_diff(time.ticks_ms(), t0) < 5000:
    v = boot.value()
    if v != last:
        print("BOOT key:", "PRESSED" if v == 0 else "released")
        last = v
    time.sleep_ms(20)

print("onboard test done")
