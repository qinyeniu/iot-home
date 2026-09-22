"""
配置管理模块
从环境变量读取所有配置
"""

import os
from typing import Optional
from pydantic import Field, model_validator
from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    """应用配置"""
    
    # MQTT 配置
    MQTT_HOST: str = "mosquitto"
    MQTT_PORT: int = 1883
    MQTT_USER: str = "iot_user"
    MQTT_PASSWORD: str = ""
    # 推荐使用后端独立账号；未配置时回退到旧 MQTT_USER/MQTT_PASSWORD。
    MQTT_BACKEND_USER: str = ""
    MQTT_BACKEND_PASSWORD: str = ""
    # 网关账号允许发布/订阅的网关段，当前固件为 gw-001。
    MQTT_GATEWAY_ID: str = "gw-001"
    MQTT_TOPIC_PREFIX: str = "iot-home"
    # 固定客户端 ID 才能使用持久会话；本地联调时可用不同 ID，避免与生产
    # 后端互相踢线。
    MQTT_CLIENT_ID: str = "iot-home-backend"
    
    # MySQL 配置
    MYSQL_HOST: str = "mysql"
    MYSQL_PORT: int = 3306
    MYSQL_DATABASE: str = "iot_home"
    MYSQL_USER: str = "iot_home"
    MYSQL_PASSWORD: str = ""
    
    # FastAPI 配置
    API_PORT: int = 8000

    # 设备在线状态：调试期约 10 秒上报，3 分钟无心跳才判离线。
    DEVICE_OFFLINE_TIMEOUT_SECONDS: int = Field(default=180, gt=0)
    DEVICE_STATUS_CHECK_INTERVAL_SECONDS: int = Field(default=20, gt=0)

    # 命令发出后的确认窗口；与超时阈值保持一致。
    COMMAND_ACK_WINDOW_SECONDS: int = Field(default=30, gt=0)
    COMMAND_TIMEOUT_SECONDS: int = Field(default=30, gt=0)
    COMMAND_TIMEOUT_CHECK_INTERVAL_SECONDS: int = Field(default=10, gt=0)
    
    # Grafana 配置
    GRAFANA_ADMIN_USER: str = "admin"
    GRAFANA_ADMIN_PASSWORD: str = ""
    
    @model_validator(mode="after")
    def validate_backend_mqtt_identity(self):
        """后端账号必须成组配置，避免用户名/密码交叉组合。"""
        has_user = bool(self.MQTT_BACKEND_USER)
        has_password = bool(self.MQTT_BACKEND_PASSWORD)
        if has_user != has_password:
            raise ValueError(
                "MQTT_BACKEND_USER and MQTT_BACKEND_PASSWORD must be configured together"
            )
        return self

    @property
    def mqtt_username(self) -> str:
        """后端连接 MQTT 的用户名：优先独立后端账号。"""
        return self.MQTT_BACKEND_USER or self.MQTT_USER

    @property
    def mqtt_password(self) -> str:
        """后端连接 MQTT 的密码：优先独立后端账号。"""
        return self.MQTT_BACKEND_PASSWORD or self.MQTT_PASSWORD

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

    @property
    def mqtt_topic_rf_policy(self) -> str:
        """射频功率策略主题"""
        return f"{self.MQTT_TOPIC_PREFIX}/+/nodes/+/rf_policy"
    
    class Config:
        env_file = ".env"
        env_file_encoding = "utf-8"


# 全局配置实例
settings = Settings()
