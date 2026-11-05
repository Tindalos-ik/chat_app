# JSON 类型、编码与协议校验

## 格式与使用位置

JSON 的值包括对象、数组、字符串、数字、布尔和 null。对象成员名为字符串，字符串中的引号、反斜杠和控制字符需要转义。网络文本使用 UTF-8；字符数与 UTF-8 字节数不同，TCP 长度字段和消息大小限制必须按最终序列化字节数计算。

| 位置 | 使用方式 | 代码入口 |
| --- | --- | --- |
| Qt 与 GateServer | HTTP body 中的 JSON | [GateServer LogicSystem](../GateServer/src/LogicSystem.cpp) |
| Qt 与 ChatServer | 自定义 TCP 帧的 JSON body | [TcpMgr](../chat_app%20desktop/tcpmgr.cpp)、[ChatServer LogicSystem](../ChatServer1/src/LogicSystem.cpp) |
| Qt 与 ResourceServer | 上传下载任务、Base64 分片与元数据 JSON | [LogicWorker](../ResourceServer/src/LogicWorker.cpp) |
| ResourceServer 磁盘 | `meta.json` 保存任务状态 | [ResourceVerifier](../ResourceServer/src/ResourceVerifier.cpp) |
| Node.js | `JSON.parse()` 读取运行配置 | [config.js](../VarifyServer/config.js) |
| 服务间 | Protobuf；部分信封含 `payload_json` | [message.proto](../proto/message.proto) |

JSON 没有内置业务 schema。增加字段能否兼容取决于接收端是否接受未知字段；字段存在、类型、取值范围、授权和业务状态均需要程序判断。JSON 编码也不提供加密或消息边界，TCP 拆包由帧协议处理，详见 [TCP服务器](TCP服务器.md)。

## Qt 的对象与字节流

`QJsonObject` 保存对象，`QJsonArray` 保存数组，`QJsonDocument` 负责文档编码和解析。

```cpp
// 将业务对象编码为 UTF-8 紧凑 JSON；HTTP/TCP 层另行设置长度和路由。
QJsonObject request;
request.insert("uid", 1001);
request.insert("token", token);
const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
```

`toJson()` 默认参数是 `QJsonDocument::Indented`，紧凑格式要显式传 `Compact`。项目中格式化与紧凑写法共存；协议体积限制应与实际发送的格式一致。JSON 空白不影响解析后的含义，但会影响帧长度。

```cpp
// error 捕获语法错误；对象检查拒绝语法正确的数组或标量。
QJsonParseError error;
const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
if (error.error == QJsonParseError::NoError && document.isObject()) {
    const QJsonObject object = document.object();
    if (object.contains("error") && object.value("error").isDouble()) {
        const int businessError = object.value("error").toInt();
    }
}
```

缺失成员产生 Undefined，`toInt()` 等转换可能给默认值。不能把“没有 error 字段”当成 `error=0`。字符串、数组、布尔等也应先判型；转换成功不等于符合业务范围。

## JsonCpp API 与所有权

五个 C++ 服务各自编译仓库内 `jsoncpp/jsoncpp.cpp`，由 CMake 将 `jsoncpp/json` 加入 include 路径。业务包括 `json.h`，使用 `Json::Value`、`CharReaderBuilder`、`parseFromStream()`、`StreamWriterBuilder`、`writeString()`；无需再次手工生成或下载源码。随仓库头文件的版本标记可在 [GateServer json.h](../GateServer/jsoncpp/json/json.h) 查看。

```cpp
// 将输入流解析为对象；解析错误与字段错误由调用方返回对应业务码。
Json::CharReaderBuilder reader;
Json::Value request;
std::string errors;
std::istringstream input(body);
const bool parsed = Json::parseFromStream(reader, input, &request, &errors);
if (parsed && request.isObject() && request.isMember("email") && request["email"].isString()) {
    const std::string email = request["email"].asString();
}
```

`parseFromStream()` 返回语法解析结果，成功后仍要检查根类型和字段。默认解析设置不等同于业务严格 schema，不能依据解析成功就信任请求。

`Json::Value` 按值管理对象、数组和字符串，无需逐成员释放。非 const 对象的 `operator[]` 会为缺失 key 创建 null 成员；const 重载返回 null 值且不插入。`isMember()`、`isString()`、`isBool()`、`isUInt64()` 等用于显式校验。

`asString()` 不会把任意对象安全地变成 JSON 文本；类型不合适可能触发库的异常或断言路径。null、布尔和数字转换规则也不代表满足业务类型。当前随仓库 JsonCpp 的 `asUInt64()` 对负数执行范围检查，不能依赖负数转换为大数的行为，更不能把转换方法当作非负整数验证器。

构造输出片段：

```cpp
// 生成紧凑文本，适合按实际字节数做帧大小判断。
Json::Value response(Json::objectValue);
response["error"] = 0;
response["messages"] = Json::Value(Json::arrayValue);
Json::StreamWriterBuilder writer;
writer["indentation"] = "";
const std::string body = Json::writeString(writer, response);
```

`toStyledString()` 生成格式化文本，当前网关和部分 TCP 响应使用它；历史分页等路径使用紧凑写入。任意 `map/vector/自定义结构` 都需要先明确转换为 `Json::Value`，JsonCpp 不会自动理解所有 C++ 业务类型。

## 64 位 ID 与精度

JSON 文本可写十进制整数，但不同语言的数字容器能力不同。JavaScript Number 和经过 double 的转换仅能连续精确表示到 `2^53-1`。JsonCpp 支持 `Json::Int64/UInt64`，可精确保存对应范围内整数；一旦先转 double，之后再转回整数无法恢复丢失位。

Qt 6 支持保存有符号 64 位整数，应用仍需避免 `toDouble()` 中转；它不能保证完整 `uint64_t` 范围可映射到 `qint64`。本项目客户端、SQLite 数据模型广泛使用有符号 `qint64`，服务端部分表与 proto 使用 uint64，完整无符号上限并非当前端到端可用范围。

当前协议具有两种实际形式：

- Qt 的历史请求、群会话 ID、版本等通过十进制字符串发送。ChatServer 的 `ParseUint64StringField()` 使用 `from_chars` 校验非空、完整消费和溢出，拒绝负数、非数字及超范围值。
- 部分服务端响应仍以 `Json::UInt64` 输出 JSON 数字，例如 `message_id`、`thread_id`、`created_at_ms`、`file_size`；客户端存在 `toInteger()` 或 `toVariant().toLongLong()` 读取路径，不能笼统写成“所有 64 位字段均为字符串”。

请求示例：

```json
{
  "thread_id": "10001",
  "after_message_id": "9007199254740993"
}
```

字段名和方向由具体消息决定。新增或迁移跨语言 ID 时，应明确范围和字符串约定，并同时更新两端解析；不能在只有一端改成字符串后假定旧端也能读取。Node proto-loader 的 `longs: String` 只影响 Protobuf 到 JavaScript 的映射，不会改变 HTTP/TCP JSON 字段。

## 当前 HTTP 字段与真实边界

| 路由 | 请求主要字段 | 成功响应主要字段 |
| --- | --- | --- |
| `/get_varifycode` | `email` | `error`、`email` |
| `/user_register` | `user`、`email`、`passwd`、`confirm`、`varifycode` | 当前回显以上字段与 `error` |
| `/user_login` | `user`、`passwd` | `error`、`uid`、`email`、`token`、`host`、`port` |
| `/user_resetpassword` | `user`、`email`、`passwd`、`varifycode` | `error` |

网关当前多处只检查语法，字段类型校验不完整。注册在调用 `RegUser()` 前构造成功响应，且没有检查其返回值；重置也未将 `UpdatePwd()` 失败映射到响应。因此响应的 `error=0` 不能在这些路径上作为数据库提交证据。

当前注册回显密码和验证码，部分日志也打印请求；这些是敏感数据暴露边界，使用和排查时不要复制实际值到文档。客户端异或密码可逆，JSON、Base64、异或都不提供传输加密；当前实现不能据此认定账号安全。

## 帧大小与不可信数据

ChatServer 历史响应按最终紧凑 JSON 字节数限制在 4 KiB 范围，并分页；只限制消息条数无法约束长文本和资源元数据的体积。ResourceServer 下载分片使用 Base64，原始字节编码后约增大为四分之三的倒数，还需预留 JSON 字段开销；资源侧设置较小分片是为满足帧限制。

文件大小、偏移、尺寸需先确认整数类型和非负，再校验上限与加法溢出。客户端提交的路径、文件名、MIME、资源 ID 均需按资源协议验证，JSON 解析不提供路径安全或访问权限。当前资源可信元数据来自 ResourceServer 的任务与实际文件核验，进一步见 [资源服务](资源服务.md)。
