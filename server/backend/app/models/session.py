"""
数据库会话管理
使用 SQLAlchemy 2.0 异步引擎
带重连机制，等待 MySQL 就绪
"""

import asyncio
import logging
from sqlalchemy.ext.asyncio import create_async_engine, AsyncSession, async_sessionmaker
from app.config import settings

logger = logging.getLogger(__name__)

# 创建异步引擎（关闭 pool_pre_ping 避免 aiomysql 兼容问题）
engine = create_async_engine(
    settings.database_url,
    echo=False,
    pool_size=5,
    max_overflow=10,
    pool_pre_ping=False,
    pool_recycle=3600
)

# 创建异步会话工厂
async_session_factory = async_sessionmaker(
    engine,
    class_=AsyncSession,
    expire_on_commit=False
)


async def get_session() -> AsyncSession:
    """获取数据库会话（依赖注入用）"""
    async with async_session_factory() as session:
        try:
            yield session
        finally:
            await session.close()


async def init_db(max_retries=30, retry_interval=2):
    """初始化数据库表（如果不存在），带重试机制"""
    from app.models.database import Base

    for attempt in range(1, max_retries + 1):
        try:
            async with engine.begin() as conn:
                await conn.run_sync(Base.metadata.create_all)
            logger.info("数据库表初始化成功")
            return
        except Exception as e:
            if attempt < max_retries:
                logger.warning(f"等待 MySQL 就绪... ({attempt}/{max_retries}): {e}")
                await asyncio.sleep(retry_interval)
            else:
                logger.error(f"MySQL 连接失败，已重试 {max_retries} 次: {e}")
                raise


async def close_db():
    """关闭数据库连接"""
    await engine.dispose()
