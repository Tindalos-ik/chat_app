-- ============================================================
-- chat_app_db 建表语句（对齐 llfc1 表结构 + 当前代码）
-- 使用前先确认 MySQL 已启动（X Protocol 端口 33060）
--
-- 用法：
--   mysqlsh -u root -h 127.0.0.1 -P 33060 --sql < create_tables.sql
--   或在你常用的数据库工具（Navicat / Workbench）里整段执行
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
--    注意：表里已有数据时不要重复执行 INSERT
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `user_id` (
  `id` int NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

-- 初始化分配器（从 1000 开始，第一个新用户拿到 1001）
INSERT INTO `user_id` VALUES (1000);

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
--    status: 0=待处理 1=已同意（可扩展 2=拒绝）
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `friend_apply` (
  `id`      bigint NOT NULL AUTO_INCREMENT,
  `from_uid` int NOT NULL,
  `to_uid`   int NOT NULL,
  `status`   smallint NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE INDEX `from_to_uid` (`from_uid`, `to_uid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

-- ============================================================
-- 附：如果已有 uid=0 的历史用户（旧 RegUser 不分配 uid 导致），
-- 建议顺手修正：
--   UPDATE `user` SET `uid` = 1000 WHERE `uid` = 0 AND `name` = 'klein';
-- 注意执行顺序：先 UPDATE user，再执行上面的 INSERT INTO user_id VALUES (1000)
-- ============================================================
