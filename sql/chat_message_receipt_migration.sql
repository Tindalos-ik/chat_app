-- 旧版 chat_message 表增加“已显示”状态。先执行一次，再部署支持显示确认的服务端版本。
-- status 仍只表示未读/已读/撤回；displayed_at 绝不能用 status 替代。
-- 新库由 sql/create_tables.sql 创建完整结构，不需要执行本迁移。
USE `chat_app_db`;

ALTER TABLE `chat_message`
  ADD COLUMN `displayed_at` DATETIME(3) NULL DEFAULT NULL
    COMMENT '接收端确认已实际显示的时间；不等同于 status=已读' AFTER `updated_at`,
  ADD KEY `idx_recv_displayed_message` (`recv_id`, `displayed_at`, `message_id`);
