"""
IoT-Home 模拟设备脚本
模拟传感器节点发送遥测数据到 MQTT
用于在硬件到货前完成端到端联调
"""

import json
import time
import random
from datetime import datetime
import paho.mqtt.client as mqtt

# MQTT 配置
MQTT_HOST = "localhost"
MQTT_PORT = 1883
MQTT_USER = "iot_user"
MQTT_PASSWORD = "iot_mqtt_2024"
MQTT_TOPIC_PREFIX = "iot-home"

# 模拟设备配置
GATEWAY_ID = "gw-001"
NODES = [
    {
        "id": "sensor-01",
        "name": "客厅温湿度传感器",
        "type": "sensor",
        "metrics": ["temperature", "humidity", "light"]
    },
    {
        "id": "sensor-02",
        "name": "卧室温湿度传感器",
        "type": "sensor",
        "metrics": ["temperature", "humidity"]
    },
    {
        "id": "switch-01",
        "name": "客厅灯光开关",
        "type": "switch",
        "metrics": ["status"]
    }
]

# 模拟数据范围
DATA_RANGES = {
    "temperature": {"min": 18.0, "max": 32.0, "unit": "°C"},
    "humidity": {"min": 30.0, "max": 80.0, "unit": "%"},
    "light": {"min": 0, "max": 1000, "unit": "lux"},
    "status": {"min": 0, "max": 1, "unit": ""}
}


def generate_metric_value(metric: str) -> float:
    """生成模拟指标值"""
    range_info = DATA_RANGES.get(metric, {"min": 0, "max": 100})
    
    if metric == "temperature":
        # 温度在舒适范围内波动
        base = 24.0
        noise = random.uniform(-2.0, 2.0)
        return round(base + noise, 1)
    elif metric == "humidity":
        # 湿度在正常范围内波动
        base = 55.0
        noise = random.uniform(-10.0, 10.0)
        return round(base + noise, 1)
    elif metric == "light":
        # 光照根据时间变化
        hour = datetime.now().hour
        if 6 <= hour <= 18:
            base = 500
            noise = random.uniform(-200, 200)
        else:
            base = 50
            noise = random.uniform(-30, 30)
        return max(0, round(base + noise, 0))
    elif metric == "status":
        # 开关状态随机
        return random.choice([0, 1])
    else:
        return random.uniform(range_info["min"], range_info["max"])


def create_telemetry_message(node_id: str, metrics: list) -> dict:
    """创建遥测数据消息"""
    data = {}
    for metric in metrics:
        data[metric] = generate_metric_value(metric)
    
    return {
        "ts": datetime.now().isoformat(),
        "data": data,
        "node_id": node_id
    }


def create_status_message(node_id: str, status: str = "online") -> dict:
    """创建设备状态消息"""
    return {
        "status": status,
        "ts": datetime.now().isoformat(),
        "node_id": node_id
    }


def on_connect(client, userdata, flags, rc):
    """连接回调"""
    if rc == 0:
        print(f"✅ 已连接到 MQTT 服务器: {MQTT_HOST}:{MQTT_PORT}")
    else:
        print(f"❌ 连接失败，错误代码: {rc}")


def on_disconnect(client, userdata, rc):
    """断开连接回调"""
    if rc != 0:
        print(f"⚠️ 意外断开连接，错误代码: {rc}")


def main():
    """主函数"""
    print("🚀 IoT-Home 模拟设备启动")
    print(f"📡 网关: {GATEWAY_ID}")
    print(f"📱 节点: {len(NODES)} 个")
    print()
    
    # 创建 MQTT 客户端
    client = mqtt.Client(client_id=f"simulator-{GATEWAY_ID}")
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    
    try:
        # 连接 MQTT 服务器
        print(f"🔗 正在连接 {MQTT_HOST}:{MQTT_PORT}...")
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
        client.loop_start()
        
        # 等待连接建立
        time.sleep(2)
        
        # 发送设备上线状态
        print("📤 发送设备上线状态...")
        for node in NODES:
            topic = f"{MQTT_TOPIC_PREFIX}/{GATEWAY_ID}/nodes/{node['id']}/status"
            message = create_status_message(node['id'], "online")
            client.publish(topic, json.dumps(message))
            print(f"  ✅ {node['name']} ({node['id']}) 已上线")
        
        print()
        print("📊 开始发送模拟数据...")
        print("   (按 Ctrl+C 停止)")
        print()
        
        # 循环发送模拟数据
        interval = 5  # 每5秒发送一次
        while True:
            for node in NODES:
                # 发送遥测数据
                topic = f"{MQTT_TOPIC_PREFIX}/{GATEWAY_ID}/nodes/{node['id']}/telemetry"
                message = create_telemetry_message(node['id'], node['metrics'])
                client.publish(topic, json.dumps(message))
                
                # 显示发送的数据
                data_str = ", ".join([f"{k}={v}" for k, v in message['data'].items()])
                print(f"  📈 {node['name']}: {data_str}")
            
            print(f"  ⏰ 等待 {interval} 秒...")
            print()
            time.sleep(interval)
            
    except KeyboardInterrupt:
        print()
        print("🛑 正在停止模拟设备...")
        
        # 发送设备离线状态
        for node in NODES:
            topic = f"{MQTT_TOPIC_PREFIX}/{GATEWAY_ID}/nodes/{node['id']}/status"
            message = create_status_message(node['id'], "offline")
            client.publish(topic, json.dumps(message))
            print(f"  ⚪ {node['name']} ({node['id']}) 已离线")
        
        # 断开连接
        client.loop_stop()
        client.disconnect()
        print("✅ 模拟设备已停止")
        
    except Exception as e:
        print(f"❌ 错误: {e}")
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
