# ResourceServer 的 JSON 任务与元数据

## 网络协议与磁盘记录

[LogicWorker.cpp](../src/LogicWorker.cpp) 解析 TCP 上传下载消息，构造任务响应，并读写任务目录中的 `meta.json`。JSON 中的上传 ID、资源 ID、偏移、总大小和完成状态描述传输任务；实际文件字节存放在文件系统，网络分片通过 Base64 放入 JSON 字符串。

[ResourceVerifier.cpp](../src/ResourceVerifier.cpp) 读取服务端保存的元数据，检查完成标志、文件存在性与实际长度，再核验资源类型。返回给 ChatServer 的资源数据走 gRPC/Protobuf，客户端 TCP JSON 与内部资源核验 RPC 分开处理。

## 任务字段校验

文件大小与偏移需要整数类型、非负、上限以及组合运算溢出检查。当前单文件上限为 100 MiB，下载原始分片上限为 2 KiB，为 Base64 和其他 JSON 字段预留空间。2 KiB 原始字节编码约为 2732 个 Base64 字符，最终还要计入 JSON key、引号等开销。

`asUInt64()` 无法代替这些校验。本目录 JsonCpp 对负数进行范围检查，错误类型可能走异常/断言路径；业务应在转换前拒绝无效值。`meta["completed"]` 需要为布尔类型，不能仅用缺键默认值推断任务已完成。

`ReadJsonFile()` 使用 `CharReaderBuilder/parseFromStream()`；`WriteJsonFile()` 将 Value 写入文件。读取成功只代表语法可解析，元数据是否与当前任务、磁盘文件和请求身份一致需要另行确认。

## 群头像的身份绑定

群头像请求带 `purpose=group_avatar`、uid/token 和 thread_id。`ThreadId()` 接受受检查的非负整数或十进制字符串；任务元数据保存 owner 与 thread 绑定。后续请求需匹配持久化任务的用途和身份，不能通过删掉 purpose 把受保护任务变成匿名任务。

[GroupAvatarDecoder.cpp](../src/GroupAvatarDecoder.cpp) 读取 meta 后检查 purpose、owner_uid、thread_id，再解码实际图片。`EchoGroupTask()` 只回传任务关联字段，不回传 token；回显字段用于关联响应，不构成授权证明。

## 资源所有权与构建

[CMakeLists.txt](../CMakeLists.txt) 编译本目录 `jsoncpp.cpp`，把 `json` 设为头文件搜索路径。[json.h](json/json.h) 的版本标记为 `1.9.8`，业务直接包含 `json.h`。`Json::Value` 管理成员和字符串；保存对其内部内容的引用时不能超过 Value 生命周期。

JSON 的非 const 下标可为缺失 key 创建 null。语法解析后应先检查对象、成员类型与范围，文件路径安全、资源访问权限和并发任务协调由业务组件负责。`meta.json` 是本机任务记录，不应包含服务密钥；文件系统路径也不应作为公开资源标识返回。

通用 API、64 位整数与 UTF-8 字节长度见 [JSON主文](../../note/json.md)，任务、恢复和下载操作见 [资源服务](../../note/资源服务.md)。
