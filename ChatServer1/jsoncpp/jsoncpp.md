# ChatServer1 的 JsonCpp 业务协议

## TCP 帧到业务对象

[CSession.cpp](../src/CSession.cpp) 处理帧边界，将完整消息交给业务；心跳请求在会话层解析。[LogicSystem.cpp](../src/LogicSystem.cpp) 用 `Json::Value` 处理登录、好友、文本、图片、文件、历史和回执等 JSON。群业务与信令也使用 JSON 对象，但服务间通知外层是 [Protobuf](../../proto/message.proto)。

JSON 不提供 TCP 消息边界。长度必须按实际序列化字节计算，中文 UTF-8 字节数不能用字符数代替。解析成功后还需根类型、成员、身份及业务权限校验，尤其不能将请求中的发送者 uid 当成已认证身份。

## ID 与历史分页

`ParseUint64StringField()` 要求字段为十进制字符串，通过 `from_chars` 校验完整消费和范围。Qt 的历史请求将 `thread_id`、`after_message_id` 写为字符串，避免浮点中转：

```json
{"thread_id":"10001","after_message_id":"9007199254740993","page_size":100}
```

服务端历史输出中的 `message_id/thread_id/created_at_ms` 等部分字段仍用 `Json::UInt64` 编码为数字。JsonCpp 能保存 64 位无符号整数，客户端使用 qint64 和 SQLite 有符号整数，完整 uint64 范围并非端到端保证。

`SerializeCompactJson()` 用 `StreamWriterBuilder` 将 indentation 设为空。历史分页按同一序列化方法计算字节，将响应控制在 4 KiB；不能按消息条数估算大小。`next_message_id/load_more` 决定客户端下一次请求，消息正文和元数据长度会影响实际一页数量。

## 可信资源字段

`BuildVerifiedImageJson()`、`BuildVerifiedFileJson()` 从已保存的消息模型构造确认和通知，字段含资源 ID、名字、MIME、大小和服务端消息 ID。图片还带宽高。客户端候选属性不直接成为权威元数据，ChatServer 通过 ResourceService 核验后再写库和构造响应。

`asUInt64()` 是数值转换，无法替代整数判型与上限判断；本目录 JsonCpp 对负数执行范围检查，错误类型可能抛异常。`isMember()`、`isString()` 等应先于转换，非 const `operator[]` 缺成员会插入 null。

## 构建与资源管理

[CMakeLists.txt](../CMakeLists.txt) 编译本目录 `jsoncpp.cpp`，将 `json` 加入头文件目录，业务包含 `json.h`。[json.h](json/json.h) 标记 `1.9.8`。`Json::Value` 自己管理成员与字符串，不能保留指向已销毁 Value 内部数据的引用。

部分响应使用 `toStyledString()`，历史使用紧凑输出，格式不应被描述为全服务统一。两台 ChatServer 的协议字段与校验规则需保持兼容，对端发送的 JSON 信封同样需要授权检查。

完整 API 和精度边界见 [JSON主文](../../note/json.md)，持久化模型见 [聊天信息存储方案](../../note/聊天信息存储方案.md)。
