-- 命令生命周期升级（已有 MySQL 数据库执行）
-- 新增：superseded（被更新命令取代）、timeout（等待节点回报超时）。
-- 本脚本可重复执行：列和索引存在时会跳过。

-- 数据库名由 mysql 命令行（MYSQL_DATABASE）指定，不在脚本内写死。
SET @db_name := DATABASE();

-- 1) metrics.received_at：命令因果判断使用服务端接收时间，而不是设备时钟。
SET @sql := (
  SELECT IF(
    COUNT(*) = 0,
    'ALTER TABLE metrics ADD COLUMN received_at DATETIME(3) NULL COMMENT ''服务端实际接收时间'' AFTER ts',
    'SELECT 1'
  )
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = @db_name
    AND TABLE_NAME = 'metrics'
    AND COLUMN_NAME = 'received_at'
);
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

UPDATE metrics
SET received_at = COALESCE(ts, created_at)
WHERE received_at IS NULL;

ALTER TABLE metrics
  MODIFY COLUMN received_at DATETIME(3) NOT NULL COMMENT '服务端实际接收时间';

-- 2) commands.status：增加 superseded / timeout。
ALTER TABLE commands
  MODIFY COLUMN status
  ENUM('pending', 'sent', 'acknowledged', 'failed', 'superseded', 'timeout')
  DEFAULT 'pending'
  COMMENT '命令状态';

-- 3) 条件创建命令出箱索引。
SET @sql := (
  SELECT IF(
    COUNT(*) = 0,
    'ALTER TABLE commands ADD INDEX idx_command_outbox (device_id, status, sent_at, id)',
    'SELECT 1'
  )
  FROM INFORMATION_SCHEMA.STATISTICS
  WHERE TABLE_SCHEMA = @db_name
    AND TABLE_NAME = 'commands'
    AND INDEX_NAME = 'idx_command_outbox'
);
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- 4) 条件创建指标服务端接收时间索引。
SET @sql := (
  SELECT IF(
    COUNT(*) = 0,
    'ALTER TABLE metrics ADD INDEX idx_metric_history (device_id, metric, received_at, id)',
    'SELECT 1'
  )
  FROM INFORMATION_SCHEMA.STATISTICS
  WHERE TABLE_SCHEMA = @db_name
    AND TABLE_NAME = 'metrics'
    AND INDEX_NAME = 'idx_metric_history'
);
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
