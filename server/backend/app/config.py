"""
配置管理模块
从环境变量读取所有配置
"""

import os
from typing import Optional
from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    """应用配置"""
    
    # MQTT 配置
    MQTT_HOST: str = "mosquitto"
    MQTT_PORT: int = 1883
    MQTT_USER: str = "iot_user"
    MQTT_PASSWORD: str = ""
    MQTT_TOPIC_PREFIX: str = "iot-home"
    
    # MySQL 配置
    MYSQL_HOST: str = "mysql"
    MYSQL_PORT: int = 3306
    MYSQL_DATABASE: str = "iot_home"
    MYSQL_USER: str = "iot_home"
    MYSQL_PASSWORD: str = ""
    
    # FastAPI 配置
    API_PORT: int = 8000
    
    # Grafana 配置
    GRAFANA_ADMIN_USER: str = "admin"
    GRAFANA_ADMIN_PASSWORD: str = ""
    
    @property
    def database_url(self) -> str:
        """构建数据库连接 URL"""
        return (
            f"mysql+aiomysql://{self.MYSQL_USER}:{self.MYSQL_PASSWORD}"
            f"@{self.MYSQL_HOST}:{self.MYSQL_PORT}/{self.MYSQL_DATABASE}"
            f"?charset=utf8mb4"
        )
    
    @property
    def mqtt_topic_telemetry(self) -> str:
        """遥测数据主题"""
        return f"{self.MQTT_TOPIC_PREFIX}/+/nodes/+/telemetry"
    
    @property
    def mqtt_topic_status(self) -> str:
        """设备状态主题"""
        return f"{self.MQTT_TOPIC_PREFIX}/+/nodes/+/status"
    
    class Config:
        env_file = ".env"
        env_file_encoding = "utf-8"


# 全局配置实例
settings = Settings()
