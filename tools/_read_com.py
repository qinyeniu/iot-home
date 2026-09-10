import serial, time, sys
s = serial.Serial("COM6", 115200, timeout=1)
end = time.time() + 55
boot=0
while time.time() < end:
    data = s.read(4096)
    if data:
        for line in data.decode("utf-8", errors="replace").splitlines():
            ls = line.strip()
            if ("iot-gw" in ls or "WiFi OK" in ls or "MQTT connected" in ls
                    or "watchdog" in ls or "rst:" in ls):
                if "Gateway v3" in ls:
                    boot+=1; print("======== BOOT #%d ========" % boot)
                print(ls)
        sys.stdout.flush()
s.close()
