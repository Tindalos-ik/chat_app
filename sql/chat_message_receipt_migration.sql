-- 已部署的 chat_message 表增加“已显示”状态。先执行一次，再部署 1036-1039 协议版本。
-- status 仍只表示未读/已读/撤回；displayed_at 绝不能用 status 替代。
USE `chat_app_db`;

ALTER TABLE `chat_message`
  ADD COLUMN `displayed_at` DATETIME(3) NULL DEFAULT NULL
    COMMENT '接收端确认已实际显示的时间；不等同于 status=已读' AFTER `updated_at`,
  ADD KEY `idx_recv_displayed_message` (`recv_id`, `displayed_at`, `message_id`);
