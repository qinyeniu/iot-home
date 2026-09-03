-- iot-home 数据库初始化脚本
-- 容器首次启动时自动执行

-- 创建数据库
CREATE DATABASE IF NOT EXISTS iot_home
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE iot_home;

-- 设备表：存储网关和终端节点信息
CREATE TABLE IF NOT EXISTS devices (
  id VARCHAR(64) NOT NULL PRIMARY KEY COMMENT '设备唯一标识（网关ID或节点ID）',
  name VARCHAR(128) NOT NULL COMMENT '设备名称',
  type ENUM('gateway', 'sensor', 'switch') NOT NULL COMMENT '设备类型',
  parent_id VARCHAR(64) DEFAULT NULL COMMENT '父设备ID（终端的父设备是网关）',
  status ENUM('online', 'offline', 'unknown') DEFAULT 'unknown' COMMENT '设备状态',
  last_seen DATETIME DEFAULT NULL COMMENT '最后在线时间',
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  INDEX idx_type (type),
  INDEX idx_parent_id (parent_id),
  INDEX idx_status (status),
  INDEX idx_last_seen (last_seen)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='设备信息表';

-- 指标表：存储所有传感器数据（核心扩展性设计）
CREATE TABLE IF NOT EXISTS metrics (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  device_id VARCHAR(64) NOT NULL COMMENT '设备ID',
  metric VARCHAR(64) NOT NULL COMMENT '指标名称（如 temperature, humidity, light）',
  value DOUBLE NOT NULL COMMENT '指标值',
  ts DATETIME(3) NOT NULL COMMENT '时间戳（毫秒精度）',
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP COMMENT '记录创建时间',
  INDEX idx_device_metric (device_id, metric),
  INDEX idx_ts (ts),
  INDEX idx_device_ts (device_id, ts),
  FOREIGN KEY (device_id) REFERENCES devices(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='指标数据表（扩展性设计：新指标无需改表）';

-- 命令表：存储下发给设备的命令
CREATE TABLE IF NOT EXISTS commands (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  device_id VARCHAR(64) NOT NULL COMMENT '目标设备ID',
  command VARCHAR(64) NOT NULL COMMENT '命令名称',
  payload JSON DEFAULT NULL COMMENT '命令参数（JSON格式）',
  status ENUM('pending', 'sent', 'acknowledged', 'failed') DEFAULT 'pending' COMMENT '命令状态',
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  sent_at DATETIME DEFAULT NULL COMMENT '发送时间',
  acknowledged_at DATETIME DEFAULT NULL COMMENT '确认时间',
  INDEX idx_device_status (device_id, status),
  INDEX idx_created_at (created_at),
  FOREIGN KEY (device_id) REFERENCES devices(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='设备命令表';

-- 插入示例网关设备
INSERT INTO devices (id, name, type, status) VALUES
  ('gw-001', '客厅网关', 'gateway', 'offline')
ON DUPLICATE KEY UPDATE name = VALUES(name);
