"""继电器测试（MicroPython 版）：IO20 低电平接通。

接线：VCC→3V3（以模块丝印为准）、GND→GND、IN→IO20。
⚠️ 不接任何负载（COM/NO 悬空），只听咔哒声/看模块指示灯。
"""

from machine import Pin
import time

relay = Pin(20, Pin.OUT)
print("RELAY TEST: low=ON(吸合), high=OFF(释放), 3 个循环")
for i in range(3):
    relay.value(0)
    print(f"cycle {i + 1}: IN=LOW -> should CLICK ON")
    time.sleep_ms(2000)
    relay.value(1)
    print(f"cycle {i + 1}: IN=HIGH -> should RELEASE")
    time.sleep_ms(1500)
print("RELAY TEST done")
