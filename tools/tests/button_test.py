"""按键测试（MicroPython 版）：IO14 输入，外接 10kΩ 上拉到 3V3。

接线：按键一脚→IO14，另一脚→GND；IO14 同时经 10kΩ 上拉到 3V3。
松开=1（高），按下=0（低）。连续采样 5 秒。
"""

from machine import Pin
import time

btn = Pin(14, Pin.IN, Pin.PULL_UP)  # 内部上拉兜底，外置 10k 也并联生效
print("BUTTON TEST: press and release the button a few times")
for i in range(25):
    v = btn.value()
    print(f"t={i * 0.2:.1f}s value={v} {'(PRESSED)' if v == 0 else ''}")
    time.sleep_ms(200)
