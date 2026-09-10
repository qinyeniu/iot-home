import paho.mqtt.client as mqtt, time
res = {"rc": None, "connected": False}
def on_connect(c, u, f, rc, *a):
    res["rc"] = rc; res["connected"] = (rc == 0)
    print("MQTT CONNACK rc =", rc)
c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
c.on_connect = on_connect
try:
    c.connect("8.163.110.27", 1883, keepalive=30)
except Exception as e:
    print("connect() raised:", e)
c.loop_start()
time.sleep(6)
c.loop_stop()
print("RESULT:", res)
