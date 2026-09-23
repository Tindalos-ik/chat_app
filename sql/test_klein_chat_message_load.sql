-- klein 聊天历史加载测试数据。
--
-- 前置条件：已执行 sql/create_tables.sql（或旧版分步流程中的基础表与聊天表脚本），且 user 表中存在：
--   klein    (接收测试消息的账号)
--   tindalos (发送测试消息的账号)
--
-- 脚本为可重复执行：只删除当前私聊会话中带专用测试前缀的旧消息，
-- 然后重新插入 60 条消息。60 条可覆盖客户端首屏 50 条与下一页 10 条的加载场景。

USE `chat_app_db`;

START TRANSACTION;

-- 获取两个测试账号的 UID。执行前请确认这两个账号存在。
SET @klein_uid = (
    SELECT `uid` FROM `user` WHERE `name` = 'klein' LIMIT 1
);
SET @sender_uid = (
    SELECT `uid` FROM `user` WHERE `name` = 'tindalos' LIMIT 1
);

-- private_chat 按较小 UID 在前保存，保证与服务端创建私聊时的规则一致。
SET @user1_id = LEAST(@klein_uid, @sender_uid);
SET @user2_id = GREATEST(@klein_uid, @sender_uid);

-- 先创建候选 chat_thread，再通过唯一索引创建或复用 private_chat。
-- 若私聊已存在，LAST_INSERT_ID(thread_id) 返回已有 thread_id；候选 thread 随后删除，
-- 不会遗留无关联的 chat_thread。
INSERT INTO `chat_thread` (`type`, `created_at`) VALUES ('private', NOW());
SET @candidate_thread_id = LAST_INSERT_ID();

INSERT INTO `private_chat` (`thread_id`, `user1_id`, `user2_id`, `created_at`)
VALUES (LAST_INSERT_ID(@candidate_thread_id), @user1_id, @user2_id, NOW())
ON DUPLICATE KEY UPDATE `thread_id` = LAST_INSERT_ID(`thread_id`);

SET @thread_id = LAST_INSERT_ID();
DELETE FROM `chat_thread`
WHERE `id` = @candidate_thread_id AND @candidate_thread_id <> @thread_id;

-- 仅清理本脚本生成的消息，绝不删除该会话中的真实聊天记录。
DELETE FROM `chat_message`
WHERE `thread_id` = @thread_id
  AND `content` LIKE '[聊天历史分页测试][klein]%';

-- 使用固定秒级间隔创建 60 条有序消息。message_id 由表的自增主键生成，
-- 客户端可借此验证首次同步、分页游标、SQLite 去重和会话摘要更新。
SET @base_time = DATE_SUB(NOW(), INTERVAL 60 MINUTE);

INSERT INTO `chat_message` (
    `thread_id`, `sender_id`, `recv_id`, `content`, `created_at`, `updated_at`, `status`
)
SELECT
    @thread_id,
    @sender_uid,
    @klein_uid,
    CONCAT('[聊天历史分页测试][klein][', LPAD(sequence_number, 2, '0'),
           '/60] tindalos 发给 klein 的测试消息'),
    DATE_ADD(@base_time, INTERVAL sequence_number SECOND),
    DATE_ADD(@base_time, INTERVAL sequence_number SECOND),
    0
FROM (
    SELECT ones.number + tens.number * 10 + 1 AS sequence_number
    FROM (
        SELECT 0 AS number UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9
    ) AS ones
    CROSS JOIN (
        SELECT 0 AS number UNION ALL SELECT 1 UNION ALL SELECT 2
        UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5
    ) AS tens
) AS test_messages
ORDER BY sequence_number;

COMMIT;

-- 执行后核对：应返回 60 条带测试前缀的消息。
SELECT
    @klein_uid AS `klein_uid`,
    @sender_uid AS `sender_uid`,
    @thread_id AS `thread_id`,
    COUNT(*) AS `test_message_count`,
    MIN(`message_id`) AS `first_message_id`,
    MAX(`message_id`) AS `last_message_id`
FROM `chat_message`
WHERE `thread_id` = @thread_id
  AND `content` LIKE '[聊天历史分页测试][klein]%';
