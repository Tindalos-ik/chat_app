-- ============================================================
-- chat_app_test_local 全量建库建表脚本（MySQL 8.0+）
-- 项目服务使用 MySQL X DevAPI（默认端口 33060）；导入本文件时使用 mysql 客户端端口（默认 3306）
--
-- 说明：
--   1. 包含账号、好友、会话、消息与回执所需的当前表结构。
--   2. friend_apply 已包含 applicant_remark；chat_message 已包含
--      图片/文件元数据及 displayed_at。
--   3. 用于全新环境初始化；重复执行不会清空数据，也不会升级已有旧表。
-- ============================================================

-- 创建数据库（如果不存在）
CREATE DATABASE IF NOT EXISTS `chat_app_test_local`
    DEFAULT CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE `chat_app_test_local`;

-- ------------------------------------------------------------
-- 1. user：账号 + 资料
--    id    = 物理主键（内部自增）
--    uid   = 业务用户ID（对外使用，由 user_id 表分配）
--    name  = 登录名（唯一）
--    email = 邮箱（唯一）
--    pwd   = 密码（客户端异或后的串，非明文）
--    desc  = 个性签名（保留字，SQL 中必须加反引号）
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `user` (
  `id`    int NOT NULL AUTO_INCREMENT,
  `uid`   int NOT NULL DEFAULT 0,
  `name`  varchar(255) NOT NULL DEFAULT '',
  `email` varchar(255) NOT NULL DEFAULT '',
  `pwd`   varchar(255) NOT NULL DEFAULT '',
  `nick`  varchar(255) NOT NULL DEFAULT '',
  `desc`  varchar(255) NOT NULL DEFAULT '',
  `sex`   int NOT NULL DEFAULT 0,
  `icon`  varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  UNIQUE INDEX `uid` (`uid`),
  UNIQUE INDEX `email` (`email`),
  INDEX `name` (`name`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

-- ------------------------------------------------------------
-- 2. user_id：业务 uid 分配器（RegUser 依赖此表）
--    注册时：UPDATE user_id SET id = id + 1; SELECT id ...
--    初始值按需调整：应 >= 现有用户的最大 uid
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `user_id` (
  `id` int NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

-- 初始化分配器：只有表为空时才插入，避免重复执行时冲突；
-- 表里已有数据时不会改变当前分配值。
INSERT INTO `user_id` (`id`)
SELECT 1000
WHERE NOT EXISTS (SELECT 1 FROM `user_id`);

-- ------------------------------------------------------------
-- 3. friend：好友关系（双向各存一行，back 是单向备注）
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `friend` (
  `id`        int UNSIGNED NOT NULL AUTO_INCREMENT,
  `self_id`   int NOT NULL,
  `friend_id` int NOT NULL,
  `back`      varchar(255) DEFAULT '',
  PRIMARY KEY (`id`),
  UNIQUE INDEX `self_friend` (`self_id`, `friend_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

-- ------------------------------------------------------------
-- 4. friend_apply：好友申请
--    status: 0=待处理 1=已同意 2=已拒绝
--    applicant_remark：申请方给被申请方设置的好友备注
--    唯一索引 (from_uid, to_uid) 保证同一方向只有一条申请记录
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `friend_apply` (
  `id`      bigint NOT NULL AUTO_INCREMENT,
  `from_uid` int NOT NULL,
  `to_uid`   int NOT NULL,
  `applicant_remark` varchar(255) NOT NULL DEFAULT '',
  `status`   smallint NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE INDEX `from_to_uid` (`from_uid`, `to_uid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

-- ------------------------------------------------------------
-- 5. chat_thread：私聊和群聊共用的会话 ID 空间
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `chat_thread` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `type`       ENUM('private', 'group') NOT NULL,
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_thread_type_created` (`type`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ------------------------------------------------------------
-- 6. private_chat：一对用户唯一对应一个私聊会话
--    写入时保证 user1_id < user2_id。
-- ------------------------------------------------------------
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

-- ------------------------------------------------------------
-- 7. group_chat：群聊信息表；群聊业务流程暂未接入
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `group_chat` (
  `thread_id`  BIGINT UNSIGNED NOT NULL,
  `name`       VARCHAR(255) NOT NULL DEFAULT '',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`thread_id`),
  CONSTRAINT `fk_group_chat_thread`
    FOREIGN KEY (`thread_id`) REFERENCES `chat_thread` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ------------------------------------------------------------
-- 8. group_chat_member：群成员与角色
-- ------------------------------------------------------------
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

-- ------------------------------------------------------------
-- 9. chat_message：所有会话的统一消息记录与增量同步游标
-- ------------------------------------------------------------
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

-- ============================================================
-- 附：如果已有 uid=0 的历史用户（旧 RegUser 不分配 uid 导致），
-- 建议顺手修正：
--   UPDATE `user` SET `uid` = 1000 WHERE `uid` = 0 AND `name` = 'klein';
-- 注意执行顺序：先修正 user，再保证 user_id 的当前值大于最大 uid
-- ============================================================

SELECT TABLE_NAME
  FROM information_schema.TABLES
  WHERE TABLE_SCHEMA = 'chat_app_test_local'
    AND TABLE_TYPE = 'BASE TABLE'
  ORDER BY TABLE_NAME;