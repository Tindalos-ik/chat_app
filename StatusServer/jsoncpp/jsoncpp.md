# StatusServer 的数据协议与 JsonCpp 依赖

## 调度接口的数据模型

[StatusServiceImpl.cpp](../src/StatusServiceImpl.cpp) 通过 Protobuf 请求和响应实现 `GetChatServer` 与 `Login`，接口见 [message.proto](../../proto/message.proto)。当前状态服务不接收 HTTP JSON 账号请求。

`GetChatServerReq` 携带 uid，响应携带 `error/host/port/token`。服务读取 Redis Hash `login_count`，选择计数较小的 ChatServer，并列时轮询；读不到的计数按最大值参与选择，全部缺失仍可能返回实例。Redis field 值是十进制字符串，用 `std::stoll` 解析，不经 JsonCpp。

`Login` 读取 `utoken_<uid>` 并比较请求 token。当前 TCP 登录在 ChatServer 直接读 Redis，StatusService 的该接口仍保留。token 用普通 SET 保存，没有 TTL 和一次性消费。

## JsonCpp 的构建位置

[CMakeLists.txt](../CMakeLists.txt) 将本目录 `jsoncpp.cpp` 编入目标，增加 `json` 头文件目录；[StatusServer.cpp](../StatusServer.cpp) 包含 `json.h` 和 `json-forwards.h`。当前启动和调度业务没有使用 JsonCpp 解析或构造网络正文，不能据此描述为 Beast HTTP 网关。

[json.h](json/json.h) 的版本标记为 `1.9.8`。该目录提供的是随仓库的 amalgamated 依赖。维护时应保持源文件和配套头文件版本一致，避免再链接另一套带相同符号的 JsonCpp 实现。

## 扩展 JSON 字段时的使用规则

若在状态服务中增加 JSON 配置或诊断数据，需明确 schema 和敏感信息范围。`Json::Value` 表示对象或数组，`parseFromStream()` 只负责解析；`isObject()`、`isMember()`、类型和范围判断仍由调用者完成。非 const `operator[]` 可以创建缺失 null 成员，不能用转换默认值推断字段有效。

uid 当前为 int，协议中的 64 位字段需要单独设计跨语言表示方式。不可将 token 或数据库密码放入可公开诊断 JSON；JSON 本身没有保密能力。

这些是新增 JSON 使用时的规则，当前接口仍使用 gRPC/Protobuf。JsonCpp API、Qt 数字精度和编码示例见 [JSON主文](../../note/json.md)，调度与服务生命周期见 [gRPC主文](../../note/grpc.md)。
