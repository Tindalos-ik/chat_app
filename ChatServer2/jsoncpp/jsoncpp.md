# ChatServer2 的 JSON 通知与协议兼容

## 本机与跨服输入

[LogicSystem.cpp](../src/LogicSystem.cpp) 处理 Qt TCP 帧内的 JSON，[ChatServiceImpl.cpp](../src/ChatServiceImpl.cpp) 处理对端 gRPC 转来的业务通知，并向本机目标 CSession 发送 JSON。两条入口均需使用本机登录状态与权限，不能因为来源是另一台服务就省略目标会话核对。

跨服消息的外层是 [message.proto](../../proto/message.proto) 中的请求和响应。文本、图片、文件的 Protobuf 字段由接收端组装为客户端 JSON；群和音视频通知还包含 `payload_json` 文本，解析后仍需按该通知类型验证。gRPC Status 为 OK 不证明客户端已经显示或读取消息，显示与已读分别由回执协议推进。

## JsonCpp 数据操作

`Json::Value` 同时表示对象、数组与标量。`CharReaderBuilder/parseFromStream()` 负责解析，`toStyledString()` 或 `StreamWriterBuilder/writeString()` 负责编码。成员缺失时，非 const `operator[]` 创建 null，const 读取不修改对象；不能把转换后的空串或 0 当成字段已提供。

```cpp
// 通知对象的字段片段；实际发送前仍需完整业务校验。
Json::Value envelope(Json::objectValue);
envelope["error"] = 0;
envelope["thread_id"] = static_cast<Json::UInt64>(threadId);
Json::StreamWriterBuilder writer;
writer["indentation"] = "";
const std::string payload = Json::writeString(writer, envelope);
```

对象与字符串由 Value 管理，编码产生的 `std::string` 需要在异步发送层按其所有权规则保存。JSON 文本没有帧边界，发送长度必须使用最终编码字节数。

## 两端字段与整数约定

当前历史请求的 `thread_id/after_message_id` 为十进制字符串，`ParseUint64StringField()` 验证范围和完整消费；部分服务端响应仍使用数字型 `Json::UInt64`。Qt 和 SQLite 业务模型使用 qint64，不能承诺支持全部 uint64 范围，也不能经 double 转换大 ID。

图片和文件通知中的元数据应来自已持久化且由 ResourceServer 核验的消息。资源 ID 与本地文件路径具有不同含义，通知不得把资源服务器本地路径暴露给客户端。历史紧凑响应按 4 KiB 字节边界分页，两台实例需保持同样的序列化计数约定。

## 构建入口与使用边界

[CMakeLists.txt](../CMakeLists.txt) 将本目录 `jsoncpp.cpp` 编入 ChatServer2，include 目录为 `json`；[json.h](json/json.h) 标记版本 `1.9.8`。日常编译直接使用仓库文件，不需要复制网关的 HTTP 示例或重新生成第三方源码。

修改消息字段时需同时维护发送端构造、接收端读取、Qt 解析和持久化读取，保留必要的类型与范围限制。JsonCpp 只提供编码和类型操作，不提供跨服鉴权、事务或可靠投递。

库 API 与编码规则见 [JSON主文](../../note/json.md)，本服与一服共有的历史/资源字段说明见 [ChatServer1 JsonCpp](../../ChatServer1/jsoncpp/jsoncpp.md)，跨服调用生命周期见 [gRPC主文](../../note/grpc.md)。
