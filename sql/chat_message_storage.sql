-- ============================================================
-- chat_app_db 聊天信息存储建表脚本（MySQL 8.0+）
--
-- 依赖：sql/create_tables.sql 已创建 chat_app_db 与 user 表。
-- 用途：服务端持久化会话和消息，并支持客户端按
--       (thread_id, message_id) 分页增量同步。
-- ============================================================

USE `chat_app_db`;

-- 所有聊天会话的统一入口；id 即业务中的 thread_id。
CREATE TABLE IF NOT EXISTS `chat_thread` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `type`       ENUM('private', 'group') NOT NULL,
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_thread_type_created` (`type`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 私聊会话。写入时应保证 user1_id < user2_id，
-- 以使同一对用户只对应一条记录和一个 thread_id。
CREATE TABLE IF NOT EXISTS `private_chat` (
  `thread_id`  BIGINT UNSIGNED NOT NULL,
  `user1_id`   BIGINT UNSIGNED NOT NULL,
  `user2_id`   BIGINT UNSIGNED NOT NULL,
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`thread_id`),
  UNIQUE KEY `uniq_private_thread` (`user1_id`, `user2_id`),
  KEY `idx_private_user1_thread` (`user1_id`, `thread_id`),
  KEY `idx_private_user2_thread` (`user2_id`, `thread_id`),
  CONSTRAINT `fk_private_chat_thread`
    FOREIGN KEY (`thread_id`) REFERENCES `chat_thread` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 群聊自身信息；thread_id 与 chat_thread 一一对应。
CREATE TABLE IF NOT EXISTS `group_chat` (
  `thread_id`  BIGINT UNSIGNED NOT NULL,
  `name`       VARCHAR(255) NOT NULL DEFAULT '',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`thread_id`),
  CONSTRAINT `fk_group_chat_thread`
    FOREIGN KEY (`thread_id`) REFERENCES `chat_thread` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 群成员与角色。idx_user_threads 用于按用户拉取其群会话列表。
CREATE TABLE IF NOT EXISTS `group_chat_member` (
  `thread_id`   BIGINT UNSIGNED NOT NULL,
  `user_id`     BIGINT UNSIGNED NOT NULL,
  `role`        TINYINT NOT NULL DEFAULT 0 COMMENT '0=成员, 1=管理员, 2=创建者',
  `joined_at`   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `muted_until` TIMESTAMP NULL DEFAULT NULL,
  PRIMARY KEY (`thread_id`, `user_id`),
  KEY `idx_user_threads` (`user_id`, `thread_id`),
  CONSTRAINT `fk_group_member_thread`
    FOREIGN KEY (`thread_id`) REFERENCES `group_chat` (`thread_id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 一条记录代表一条消息。message_id 是全局单调递增的同步游标；
-- 客户端本地以它作为主键，可实现幂等写入与去重。
CREATE TABLE IF NOT EXISTS `chat_message` (
  `message_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `thread_id`  BIGINT UNSIGNED NOT NULL,
  `sender_id`  BIGINT UNSIGNED NOT NULL,
  `recv_id`    BIGINT UNSIGNED NOT NULL DEFAULT 0
               COMMENT '私聊接收者 uid；群聊消息固定为 0',
  `client_msg_id` VARCHAR(128) NULL
               COMMENT '客户端消息 UUID；图片/文件历史回放时保留原始 msgid',
  `message_type` ENUM('text', 'image', 'file', 'system') NOT NULL DEFAULT 'text'
               COMMENT 'text=文本，image/file=ResourceServer 已核验资源，system=系统消息',
  `content`    TEXT NOT NULL,
  `resource_id` VARCHAR(160) NULL
               COMMENT 'image/file 消息对应的 ResourceServer upload_id，绝不保存本机路径',
  `resource_name` VARCHAR(255) NULL COMMENT '由 ResourceServer 返回的可信文件名',
  `mime_type` VARCHAR(64) NULL COMMENT 'ResourceServer 返回的可信 MIME',
  `file_size` BIGINT UNSIGNED NULL COMMENT '已发布文件实际字节数',
  `width` INT UNSIGNED NULL COMMENT '已发布图片的像素宽度',
  `height` INT UNSIGNED NULL COMMENT '已发布图片的像素高度',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
               ON UPDATE CURRENT_TIMESTAMP,
  `displayed_at` DATETIME(3) NULL DEFAULT NULL
               COMMENT '接收端确认已实际显示的时间；不等同于 status=已读',
  `status`     TINYINT NOT NULL DEFAULT 0
               COMMENT '0=未读, 1=已读, 2=已撤回',
  PRIMARY KEY (`message_id`),
  KEY `idx_thread_created` (`thread_id`, `created_at`),
  KEY `idx_thread_message` (`thread_id`, `message_id`),
  KEY `idx_recv_status_message` (`recv_id`, `status`, `message_id`),
  KEY `idx_recv_displayed_message` (`recv_id`, `displayed_at`, `message_id`),
  KEY `idx_message_type_id` (`message_type`, `message_id`),
  CONSTRAINT `fk_message_thread`
    FOREIGN KEY (`thread_id`) REFERENCES `chat_thread` (`id`)
    ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 增量同步查询示例：
-- SELECT *
-- FROM `chat_message`
-- WHERE `thread_id` = ? AND `message_id` > ?
-- ORDER BY `message_id` ASC
-- LIMIT 1000;
