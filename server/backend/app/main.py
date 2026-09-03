"""
IoT-Home FastAPI 后端主应用
MQTT 订阅 + 数据入库 + REST API
"""

import asyncio
import logging
from contextlib import asynccontextmanager
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from app.config import settings
from app.models.session import init_db, close_db
from app.services.mqtt import mqtt_service
from app.routers.devices import router as devices_router

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)
logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    """应用生命周期管理"""
    logger.info("正在启动 IoT-Home 后端服务...")
    
    # 初始化数据库
    await init_db()
    logger.info("数据库初始化完成")
    
    # 启动 MQTT 客户端（后台任务）
    mqtt_task = asyncio.create_task(mqtt_service.start())
    logger.info("MQTT 客户端已启动")
    
    yield
    
    # 关闭 MQTT 客户端
    await mqtt_service.stop()
    mqtt_task.cancel()
    try:
        await mqtt_task
    except asyncio.CancelledError:
        pass
    logger.info("MQTT 客户端已停止")
    
    # 关闭数据库连接
    await close_db()
    logger.info("数据库连接已关闭")
    
    logger.info("IoT-Home 后端服务已停止")


# 创建 FastAPI 应用
app = FastAPI(
    title="IoT-Home API",
    description="IoT 三层架构学习原型 - 服务端 API",
    version="1.0.0",
    lifespan=lifespan
)

# 配置 CORS
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # 开发环境允许所有来源
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# 注册路由
app.include_router(devices_router)


@app.get("/")
async def root():
    """根路径"""
    return {
        "message": "IoT-Home API",
        "version": "1.0.0",
        "docs": "/docs",
        "health": "/api/health"
    }
