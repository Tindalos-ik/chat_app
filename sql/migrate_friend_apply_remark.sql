-- 已有 chat_app_db 数据库的增量迁移。
-- MySQL 8.0.29+ 支持 IF NOT EXISTS，可安全重复执行。
ALTER TABLE `friend_apply`
  ADD COLUMN IF NOT EXISTS `applicant_remark` varchar(255) NOT NULL DEFAULT ''
  AFTER `to_uid`;
