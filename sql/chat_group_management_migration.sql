-- 已部署 role0/1/2 群库增量升级：停写备份后在目标 schema 执行一次。
-- 不执行 chat_group_migration.sql，不重写任何现有角色、成员或消息。
ALTER TABLE group_chat
  ADD COLUMN version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  ADD COLUMN approval_required TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN announcement TEXT NOT NULL DEFAULT (''),
  ADD COLUMN announcement_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN announcement_author_uid BIGINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN announcement_updated_at DATETIME(3) NULL,
  ADD COLUMN avatar_resource_id VARCHAR(160) NOT NULL DEFAULT '',
  ADD COLUMN avatar_mime_type VARCHAR(64) NOT NULL DEFAULT '',
  ADD COLUMN avatar_size BIGINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN avatar_width INT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN avatar_height INT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN avatar_version BIGINT UNSIGNED NOT NULL DEFAULT 0;
ALTER TABLE group_chat_member
  ADD COLUMN active TINYINT NOT NULL DEFAULT 1,
  ADD COLUMN membership_version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  DROP CHECK chk_group_role,
  ADD CONSTRAINT chk_group_role CHECK (role IN (0,1,2)),
  MODIFY role TINYINT NOT NULL DEFAULT 0 COMMENT '0=成员, 1=管理员, 2=群主';

-- 全局准入游标；唯一待处理约束跨邀请/申请，终态保留用于离线恢复。
CREATE TABLE IF NOT EXISTS `group_admission` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `thread_id` BIGINT UNSIGNED NOT NULL,
  `kind` ENUM('invitation','application') NOT NULL,
  `user_id` BIGINT UNSIGNED NOT NULL,
  `inviter_uid` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `status` ENUM('pending','accepted','approved','rejected','invalid') NOT NULL DEFAULT 'pending',
  `explanation` VARCHAR(256) NOT NULL DEFAULT '',
  `created_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  `updated_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  `pending_uid` BIGINT UNSIGNED GENERATED ALWAYS AS (IF(status='pending',user_id,NULL)) STORED,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_group_pending` (`thread_id`,`pending_uid`),
  KEY `idx_group_admission_user` (`user_id`,`id`),
  KEY `idx_group_admission_thread` (`thread_id`,`id`),
  CONSTRAINT `fk_group_admission_thread` FOREIGN KEY (`thread_id`) REFERENCES `group_chat` (`thread_id`) ON DELETE RESTRICT,
  CONSTRAINT `chk_group_admission_state` CHECK (
    (kind='invitation' AND status IN ('pending','accepted','rejected','invalid')) OR
    (kind='application' AND status IN ('pending','approved','rejected','invalid')))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 业务重放键保留精确输入和正式结果；request_id 不是幂等键。
CREATE TABLE IF NOT EXISTS `group_operation` (
  `user_id` BIGINT UNSIGNED NOT NULL,
  `operation_id` VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `action` SMALLINT NOT NULL,
  `input_json` TEXT NOT NULL,
  `result_json` TEXT NOT NULL,
  `created_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`user_id`,`operation_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

