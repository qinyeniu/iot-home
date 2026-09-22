"""临时硬件实测服务器：本地启动后端，经远程 MQTT 控制真实节点。

- SQLite 本地文件，不依赖 MySQL；
- MQTT 客户端 ID 使用 localdev，不与生产后端互踢；
- 匿名连接（与 tools/rf_power_config.py 一致）。
仅用于人工/自动化硬件验证，不属于交付物。
"""

import asyncio
import os
import sys
from pathlib import Path

BACKEND_DIR = Path(__file__).resolve().parent.parent / "server" / "backend"
sys.path.insert(0, str(BACKEND_DIR))

# Windows + Python 3.14 下 uvicorn 默认 Proactor 循环不支持 paho 使用的
# add_reader/add_writer，必须切换到 Selector 循环。
if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

DB_PATH = Path(__file__).resolve().parent.parent / "tools" / "_local_hw_test.db"

# 先替换数据库引擎，再导入依赖它的模块。
from sqlalchemy.ext.asyncio import create_async_engine, AsyncSession, async_sessionmaker

engine = create_async_engine(f"sqlite+aiosqlite:///{DB_PATH}")
factory = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)

import app.models.session as session_mod

session_mod.engine = engine
session_mod.async_session_factory = factory

from app.config import settings

settings.MQTT_HOST = "8.163.110.27"
settings.MQTT_PORT = 1883
settings.MQTT_CLIENT_ID = "iot-home-backend-localdev"
settings.MQTT_USER = None
settings.MQTT_PASSWORD = None

import uvicorn

async def prepare():
    from app.models.database import Base, Device

    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
    async with factory() as session:
        if await session.get(Device, "gw-001-zb-82cb") is None:
            session.add(
                Device(
                    id="gw-001-zb-82cb",
                    name="zb-82cb",
                    type="sensor",
                    status="online",
                )
            )
            await session.commit()

asyncio.run(prepare())

from app.main import app

# loop="none" 让 uvicorn 不要自己创建/替换循环；由 asyncio.run 在
# SelectorEventLoopPolicy 下提供循环，否则 aiomqtt 的 add_reader 在
# Proactor 循环上会失败（uvicorn 在 Windows 上会强制 Proactor 策略）。
config = uvicorn.Config(
    app, host="127.0.0.1", port=8011, loop="none", log_level="info"
)
server = uvicorn.Server(config)
asyncio.run(server.serve())
