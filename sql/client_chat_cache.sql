-- ============================================================
-- 客户端聊天缓存建表脚本（SQLite 3）
--
-- 建议每个已登录用户使用独立文件，例如：chat_cache_<uid>.db。
-- 不要将不同账号的消息保存在同一个未隔离的缓存库中。
--
-- 执行本脚本后，每次打开数据库连接仍应执行：
-- PRAGMA foreign_keys = ON;
-- ============================================================

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

-- 本机已知的会话，用于快速渲染聊天列表。
CREATE TABLE IF NOT EXISTS local_chat_thread (
  thread_id            INTEGER PRIMARY KEY,
  thread_type          TEXT NOT NULL CHECK (thread_type IN ('private', 'group')),
  title                TEXT NOT NULL DEFAULT '',
  peer_uid             INTEGER NOT NULL DEFAULT 0,
  last_message_id      INTEGER NOT NULL DEFAULT 0,
  last_message_preview TEXT NOT NULL DEFAULT '',
  last_message_at_ms   INTEGER NOT NULL DEFAULT 0,
  unread_count         INTEGER NOT NULL DEFAULT 0 CHECK (unread_count >= 0),
  updated_at_ms        INTEGER NOT NULL DEFAULT 0
);

-- 每个会话各自维护同步游标。登录时将 thread_id 与 max_message_id
-- 发送给服务端，以拉取该会话之后的消息。
CREATE TABLE IF NOT EXISTS local_sync_cursor (
  thread_id      INTEGER PRIMARY KEY,
  max_message_id INTEGER NOT NULL DEFAULT 0 CHECK (max_message_id >= 0),
  synced_at_ms   INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (thread_id) REFERENCES local_chat_thread (thread_id)
    ON DELETE CASCADE
);

-- 本地消息缓存。message_id 直接使用服务端的全局消息 ID，
-- INSERT OR IGNORE 可安全处理重复推送和重复同步。
CREATE TABLE IF NOT EXISTS local_chat_message (
  message_id    INTEGER PRIMARY KEY,
  thread_id     INTEGER NOT NULL,
  sender_id     INTEGER NOT NULL,
  recv_id       INTEGER NOT NULL DEFAULT 0,
  content_type  TEXT NOT NULL DEFAULT 'text'
                CHECK (content_type IN ('text', 'image', 'audio', 'file', 'system')),
  content       TEXT NOT NULL DEFAULT '',
  created_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL,
  server_status INTEGER NOT NULL DEFAULT 0
                CHECK (server_status IN (0, 1, 2)),
  send_state    INTEGER NOT NULL DEFAULT 1
                CHECK (send_state IN (0, 1, 2, 3)),
  delivery_state INTEGER NOT NULL DEFAULT 1
                CHECK (delivery_state IN (1, 2, 3, 4)),
  is_read       INTEGER NOT NULL DEFAULT 1 CHECK (is_read IN (0, 1)),
  FOREIGN KEY (thread_id) REFERENCES local_chat_thread (thread_id)
    ON DELETE CASCADE
);

-- 聊天窗口历史消息分页：
-- WHERE thread_id = ? AND message_id < ? ORDER BY message_id DESC LIMIT ?
CREATE INDEX IF NOT EXISTS idx_local_message_thread_id
  ON local_chat_message (thread_id, message_id DESC);

-- 由时间定位、清理过期缓存时使用。
CREATE INDEX IF NOT EXISTS idx_local_message_created_at
  ON local_chat_message (created_at_ms);

-- 附件本体保存在客户端文件目录；数据库只保存路径和下载状态。
CREATE TABLE IF NOT EXISTS local_message_attachment (
  attachment_id INTEGER PRIMARY KEY,
  message_id    INTEGER NOT NULL,
  local_path    TEXT NOT NULL DEFAULT '',
  remote_url    TEXT NOT NULL DEFAULT '',
  mime_type     TEXT NOT NULL DEFAULT '',
  file_size     INTEGER NOT NULL DEFAULT 0 CHECK (file_size >= 0),
  download_state INTEGER NOT NULL DEFAULT 0 CHECK (download_state IN (0, 1, 2, 3)),
  FOREIGN KEY (message_id) REFERENCES local_chat_message (message_id)
    ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_local_attachment_message
  ON local_message_attachment (message_id);

-- 写入一批服务端消息时，应置于同一个事务内。示例：
-- BEGIN;
-- INSERT OR IGNORE INTO local_chat_message (...)
-- VALUES (...);
-- INSERT INTO local_sync_cursor (thread_id, max_message_id, synced_at_ms)
-- VALUES (?, ?, ?)
-- ON CONFLICT(thread_id) DO UPDATE SET
--   max_message_id = MAX(max_message_id, excluded.max_message_id),
--   synced_at_ms = excluded.synced_at_ms;
-- COMMIT;
