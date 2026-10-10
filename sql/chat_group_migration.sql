-- 已部署旧群表升级；先备份，在服务停止写入时对目标 schema 执行一次。
-- 不删除任何会话/消息；旧表可能只有预留记录，选原创建者或最小 UID 为群主。
ALTER TABLE group_chat ADD COLUMN owner_uid BIGINT UNSIGNED NOT NULL DEFAULT 0,
    ADD COLUMN active TINYINT NOT NULL DEFAULT 1;
UPDATE group_chat g LEFT JOIN (
    SELECT thread_id, COALESCE(MIN(CASE WHEN role=2 THEN user_id END),MIN(user_id)) AS owner_uid
    FROM group_chat_member GROUP BY thread_id
) m ON m.thread_id=g.thread_id
SET g.owner_uid=COALESCE(m.owner_uid,0), g.active=IF(m.owner_uid IS NULL,0,1);
UPDATE group_chat_member m JOIN group_chat g ON g.thread_id=m.thread_id
SET m.role=IF(m.user_id=g.owner_uid,2,0);
ALTER TABLE group_chat DROP FOREIGN KEY fk_group_chat_thread;
ALTER TABLE group_chat ADD CONSTRAINT fk_group_chat_thread
    FOREIGN KEY(thread_id) REFERENCES chat_thread(id) ON DELETE RESTRICT;
ALTER TABLE group_chat_member DROP FOREIGN KEY fk_group_member_thread;
ALTER TABLE group_chat_member ADD CONSTRAINT fk_group_member_thread
    FOREIGN KEY(thread_id) REFERENCES group_chat(thread_id) ON DELETE RESTRICT,
    ADD CONSTRAINT chk_group_role CHECK(role IN (0,2));
ALTER TABLE group_chat_member MODIFY role TINYINT NOT NULL DEFAULT 0 COMMENT '0=成员, 2=群主';
