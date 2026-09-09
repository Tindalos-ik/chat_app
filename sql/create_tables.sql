-- ============================================================
-- chat_app_db 建表脚本（最终结构）
-- 使用前先确认 MySQL 已启动（X Protocol 端口 33060）
--
-- 说明：
--   1. 四张表的结构与当前代码一致，friend_apply 已直接包含
--      applicant_remark 字段。
--   2. 本脚本用于全新环境建库；对已有库重复执行时，只会保留
--      已存在的数据，不会修改旧表结构。
-- ============================================================

-- 创建数据库（如果不存在）
CREATE DATABASE IF NOT EXISTS `chat_app_db`
    DEFAULT CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE `chat_app_db`;

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

-- ============================================================
-- 附：如果已有 uid=0 的历史用户（旧 RegUser 不分配 uid 导致），
-- 建议顺手修正：
--   UPDATE `user` SET `uid` = 1000 WHERE `uid` = 0 AND `name` = 'klein';
-- 注意执行顺序：先修正 user，再保证 user_id 的当前值大于最大 uid
-- ============================================================
