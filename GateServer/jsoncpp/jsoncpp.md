# GateServer 的 JsonCpp 请求与响应

## HTTP 数据流

[LogicSystem.cpp](../src/LogicSystem.cpp) 从 Beast request body 取字节字符串，用 `CharReaderBuilder` 和 `parseFromStream()` 解析到 `Json::Value`，按 HTTP 路由读取字段，再构造响应对象并 `toStyledString()` 写入 Beast response body。

HTTP body 的语法解析与业务校验相互独立：语法正确的数组、null、错误类型或缺字段均不能直接当成合法账号请求。当前网关校验不完整，`/get_varifycode` 检查 email 是否存在，其他多处直接调用 `asString()`；新增处理应明确检查根对象、成员类型和业务范围。

## 解析与编码 API

```cpp
// body 来自 HTTP 请求；parsed 只说明 JSON 语法解析结果。
Json::Value request;
Json::CharReaderBuilder reader;
std::string errors;
std::istringstream input(body);
const bool parsed = Json::parseFromStream(reader, input, &request, &errors);
```

非 const `request["email"]` 在缺成员时插入 null；const 读取不会插入。`isMember()` 与 `isString()` 用于显式检查，`asString()` 不负责判断邮箱格式。`Json::Value` 自行管理字符串和成员内存，业务无需手动释放其子节点。

响应中的 `error` 为业务状态，HTTP 成功收到正文仍需检查该字段。登录成功输出 `uid/email/token/host/port`。当前注册响应还回显密码和验证码，运维与客户端日志应避免记录这些实际值。

## 编译接入

[CMakeLists.txt](../CMakeLists.txt) 把本目录 `jsoncpp.cpp` 加入可执行目标，将 `json` 目录加入 include 搜索路径，业务使用 `#include "json.h"`。头文件 [json.h](json/json.h) 标记版本 `1.9.8`。随仓库的 amalgamated 源码已经可用于构建，无需在日常配置时重新运行生成脚本，也不要同时链接另一份重复实现。

## 当前使用边界

JSON 转换不提供认证或保密。客户端密码异或可逆，序列化不能代替 TLS。网关部分日志打印整个请求 body，排查时需保护密码、验证码和 token。

注册与重置响应没有可靠传递所有数据库失败：注册未检查 `RegUser()` 返回值，重置未检查 `UpdatePwd()` 返回值，`error=0` 不能作为事务成功证据。完整字段、类型转换和客户端默认值规则见 [JSON主文](../../note/json.md)，账号事务见 [网关MySQL](../docx/mysql.md)。
