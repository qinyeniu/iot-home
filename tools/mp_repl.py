"""MicroPython REPL 助手：把脚本发送到板子并打印输出。

用法（在项目根目录）：
    python tools/mp_repl.py <串口号> <micropython 脚本路径>
示例：
    python tools/mp_repl.py COM5 tools/tests/aht20_read.py
"""

import sys
import time

import serial


def main() -> None:
    if len(sys.argv) != 3:
        print("用法: python tools/mp_repl.py <串口号> <脚本路径>")
        sys.exit(1)
    port = sys.argv[1]
    script_path = sys.argv[2]
    with open(script_path, "r", encoding="utf-8") as f:
        script = f.read()

    p = serial.Serial(port, 115200, timeout=0.3)
    try:
        time.sleep(1)
        p.write(b"\x03")  # Ctrl+C：中断板子上可能正在运行的程序
        time.sleep(0.3)
        p.reset_input_buffer()
        p.write(b"\x05")  # Ctrl+E：进入粘贴模式，避免空行被当成执行
        time.sleep(0.2)
        p.write(script.encode("utf-8"))
        p.write(b"\x04")  # Ctrl+D：结束粘贴并执行
        # 持续读取直到板子回到 REPL 提示符（>>>），最长等 25 秒
        out = bytearray()
        deadline = time.time() + 25
        while time.time() < deadline:
            chunk = p.read(4096)
            if chunk:
                out.extend(chunk)
                if out[-20:].find(b">>>") != -1:
                    break
            else:
                time.sleep(0.05)
        # 直接写原始字节，避免 Windows 控制台 GBK 编码打印 Unicode 符号报错
        sys.stdout.buffer.write(bytes(out))
        sys.stdout.buffer.write(b"\r\n")
    finally:
        p.close()


if __name__ == "__main__":
    main()
