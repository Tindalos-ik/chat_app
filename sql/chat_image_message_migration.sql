-- ============================================================
-- 已部署 chat_message 的图片消息迁移（MySQL 8.0+）
--
-- 前置：chat_message_storage.sql 已执行，且当前库为 chat_app_db。
-- 新部署请直接执行更新后的 chat_message_storage.sql；已有库执行本文件。
--
-- 不使用 CREATE INDEX IF NOT EXISTS：MySQL 不支持这条语法。
-- 每项变更先查询 information_schema，再用 PREPARE 执行，故脚本可安全重跑，
-- 也能修复“字段已加完、创建索引时报语法错误”的半完成状态。
-- ============================================================

USE `chat_app_db`;

-- 约定：每一段的 @sql 不是空字符串，而是无副作用的 SELECT 1。这样 PREPARE
-- 在列或索引已经存在时仍有合法语句可执行。

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'client_msg_id') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `client_msg_id` VARCHAR(128) NULL COMMENT ''客户端消息 UUID；图片历史回放时保留原始 msgid'' AFTER `recv_id`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'message_type') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `message_type` ENUM(''text'', ''image'', ''file'', ''system'') NOT NULL DEFAULT ''text'' COMMENT ''text=文本，image/file=ResourceServer 已核验资源，system=系统消息'' AFTER `client_msg_id`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

-- 文件消息复用资源字段，图片尺寸列保持 NULL；兼容 message_type 尚未创建的旧库。
SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'message_type') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `message_type` ENUM(''text'', ''image'', ''file'', ''system'') NOT NULL DEFAULT ''text'' COMMENT ''text=文本，image/file=ResourceServer 已核验资源，system=系统消息'' AFTER `client_msg_id`',
  IF(
    (SELECT COLUMN_TYPE FROM information_schema.COLUMNS
     WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
       AND COLUMN_NAME = 'message_type') NOT LIKE '%file%',
    'ALTER TABLE `chat_message` MODIFY COLUMN `message_type` ENUM(''text'', ''image'', ''file'', ''system'') NOT NULL DEFAULT ''text'' COMMENT ''text=文本，image/file=ResourceServer 已核验资源，system=系统消息''',
    'SELECT 1'));
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'resource_id') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `resource_id` VARCHAR(160) NULL COMMENT ''ResourceServer upload_id，不保存资源服务绝对路径'' AFTER `content`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'resource_name') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `resource_name` VARCHAR(255) NULL COMMENT ''ResourceServer 可信文件名'' AFTER `resource_id`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'mime_type') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `mime_type` VARCHAR(64) NULL COMMENT ''ResourceServer 按文件魔数识别的 MIME'' AFTER `resource_name`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'file_size') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `file_size` BIGINT UNSIGNED NULL COMMENT ''已发布文件的实际字节数'' AFTER `mime_type`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'width') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `width` INT UNSIGNED NULL COMMENT ''图片宽度'' AFTER `file_size`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND COLUMN_NAME = 'height') = 0,
  'ALTER TABLE `chat_message` ADD COLUMN `height` INT UNSIGNED NULL COMMENT ''图片高度'' AFTER `width`',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;

SET @sql = IF(
  (SELECT COUNT(*) FROM information_schema.STATISTICS
   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
     AND INDEX_NAME = 'idx_message_type_id') = 0,
  'ALTER TABLE `chat_message` ADD INDEX `idx_message_type_id` (`message_type`, `message_id`)',
  'SELECT 1');
PREPARE chat_image_migration_stmt FROM @sql;
EXECUTE chat_image_migration_stmt;
DEALLOCATE PREPARE chat_image_migration_stmt;
