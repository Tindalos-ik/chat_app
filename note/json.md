# JSON 使用笔记（客户端 + 后端）

> 覆盖范围：
> * **Qt 客户端**（`chat_app desktop/`）：`QJsonObject` / `QJsonDocument`，负责 HTTP 请求与 TCP 消息的序列化/反序列化
> * **C++ 服务端**（GateServer / ChatServer）：**jsoncpp**（`Json::Value`），负责解析客户端请求、构造回包
> * **Node.js**（VarifyServer）：只把 JSON 用于读 `config.json`，协议层走 gRPC（protobuf），不涉及 JSON 通信
>
> 登录完整链路见 [login.md](login.md)。

---

### 为什么用 JSON 做通信格式

这个项目的通信格式有三种，各司其职：

| 场景 | 格式 | 原因 |
| --- | --- | --- |
| 客户端 ↔ GateServer（HTTP） | JSON | 跨语言（C++/Node.js）、人可读、调试方便 |
| 客户端 ↔ ChatServer（TCP 消息体） | JSON | 同上，消息头仍是二进制（id + len） |
| 服务间（StatusServer 等） | protobuf（gRPC） | 性能好、有 schema 强约束 |
| 密码等敏感字段 | JSON 值 + 异或混淆 | 简单防明文，见 [数据库设计.md](数据库设计.md) 的说明 |

JSON 的取舍：

* **优点**：跨语言、可读、**加字段不破坏旧端**（宽松协议，收到不认识的多余字段直接忽略）；
* **缺点**：体积大（键名重复）、解析慢、**无 schema**（拼错键名双方都不知道）。

---

### 客户端（Qt）用法

Qt 的 JSON 核心就两个类：

* `QJsonObject`：一个 JSON 对象（键值对），对应 `{...}`；
* `QJsonDocument`：负责对象和字节流之间的转换（序列化/反序列化）。

#### 序列化：QJsonObject → 字节流（发请求）

```cpp
// 1. 组装对象
QJsonObject json_obj;
json_obj["user"] = ui->user_edit->text();
json_obj["passwd"] = xorString(ui->pwd_edit->text());   // 密码先异或再传输

// 2. 转成字节流（默认 Compact 紧凑格式）
QByteArray data = QJsonDocument(json_obj).toJson();

// 3. 设置请求头，发 HTTP POST
QNetworkRequest request(url);
request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(data.length()));
QNetworkReply* reply = _manager.post(request, data);
```

要点：

* `QJsonDocument(json_obj).toJson()` 默认是**紧凑格式**（`{"user":"a","passwd":"b"}`），适合网络传输；
* `Content-Type` 告诉服务器"请求体是 JSON"，`Content-Length` 告诉服务器读多少字节；
* 赋值直接 `json_obj["键"] = 值`，字符串、int、bool 都支持，自动转成 `QJsonValue`。

#### 反序列化：字节流 → QJsonObject（收回包）

```cpp
void RegisterDialog::slot_reg_mod_finish(ReqId id, QString res, ErrorCodes err)
{
    // 1. 字符串 → UTF-8 字节 → JSON 文档
    QJsonDocument jsonDoc = QJsonDocument::fromJson(res.toUtf8());

    // 2. 双重检查：解析失败 或 不是对象
    if(jsonDoc.isNull() || !jsonDoc.isObject()){
        showtip(tr("json解析失败"),"err");
        return;
    }

    // 3. 文档 → 对象，交给 handler 按 ReqId 分发
    _handlers[id](jsonDoc.object());
}
```

取值：

```cpp
int err = jsonObj["error"].toInt();            // 数字
QString email = jsonObj["email"].toString();   // 字符串
if(jsonObj.contains("error")) { ... }          // 判断键是否存在
```

> **大坑（客户端）**：`QJsonValue` 在键不存在时返回**安全默认值**——`toInt()` 返回 0、`toString()` 返回空串。这不会崩，但会**悄悄掩盖错误**：比如服务器回包漏了 `"error"` 字段，你拿到 0 还以为成功。严格场景建议先 `contains()` 再取值（TcpMgr 就是这么做的）。

#### TCP 消息里的 JSON（tcpmgr.cpp）

发送时它用了**缩进格式**：

```cpp
QJsonDocument doc(json_obj);
QString jsonString = doc.toJson(QJsonDocument::Indented);  // 带缩进、换行
```

接收时先 `QJsonDocument::fromJson(data)`，然后检查 `isNull()` 和 `contains("error")`：

```cpp
if(jsonDoc.isNull()){
    emit sig_login_failed(ErrorCodes::ERR_JSON);
    return;
}
QJsonObject json_obj = jsonDoc.object();
if(!json_obj.contains("error")){ ... }
```

> 小知识点：`Indented` 格式每个键一行、带缩进，人眼好读，但**体积和带宽都更浪费**。HTTP 请求用 Compact 是对的，TCP 消息体其实也应该用 Compact（`toJson()` 默认），教学项目无所谓，知道差别即可。

---

### C++ 服务端（jsoncpp）用法

jsoncpp 的核心是 `Json::Value`——一个万能 JSON 容器，数组、对象、字符串、数字都是它。

#### 解析请求

```cpp
Json::Value src_root;                 // 解析结果放这里
Json::CharReaderBuilder reader;       // 解析器
std::istringstream ss(body_str);      // 请求体转成流
std::string errs;

bool parse_success = Json::parseFromStream(reader, ss, &src_root, &errs);
if(!parse_success){
    root["error"] = ErrorCode::Error_Json;   // 解析失败返回错误码
    ...
}
```

#### 取键

```cpp
std::string user  = src_root["user"].asString();
std::string email = src_root["email"].asString();
std::string varifycode = src_root["varifycode"].asString();
```

> **大坑（服务端）**：jsoncpp 的 `[]` 运算符有个危险特性——**键不存在时会自动创建一个空成员**。也就是说拼错键名不会报错，而是悄悄往 `src_root` 里塞了一个新键，取出来是空字符串。排查"为什么字段一直是空的"时，先检查键名有没有拼错，或改用 `isMember("user")` 判断。

#### 构造回包

```cpp
Json::Value root;
root["error"] = ErrorCode::Success;      // 数字
root["uid"] = user_info.uid;
root["email"] = user_info.email;
root["token"] = reply.token();
root["host"] = reply.host();
root["port"] = reply.port();

std::string jsonstr = root.toStyledString();  // 转成字符串（带缩进）
beast::ostream(connection->_response.body()) << jsonstr;
```

`toStyledString()` 相当于 Qt 的 `Indented`，也是给人看的格式，项目里服务端回包都这么发。

ChatServer 里配合 `Defer` 自动发送回包（登录成功才填用户信息，失败直接 return 也会发 error）：

```cpp
Json::Value rtvalue;
Defer defer([this, &rtvalue, session]{
    std::string return_str = rtvalue.toStyledString();
    session->Send(return_str, MSG_CHAT_LOGIN_RSP);
});

rtvalue["error"] = rsp.error();
if(rsp.error() != ErrorCode::Success) return;   // 直接走 Defer 发错误回包
// 填用户信息...
```

---

### Node.js（VarifyServer）

VarifyServer **不参与 JSON 通信**——它对外是 gRPC 服务，协议是 protobuf。JSON 只出现在配置文件：

```js
let config = JSON.parse(fs.readFileSync('config.json', 'utf8')); // 读配置
```

所以 JSON 的跨语言互操作主要体现在"Qt 客户端 ↔ C++ 网关/聊天服务器"这一段，Node.js 那段由 protobuf 负责。

---

### 协议字段约定（JSON 版）

#### HTTP 请求（客户端 → GateServer）

| 路由 | 字段 |
| --- | --- |
| `/get_varifycode` | `email` |
| `/user_register` | `user`, `email`, `passwd`(异或), `confirm`(异或), `varifycode` |
| `/user_login` | `user`, `passwd`(异或) |
| `/user_resetpassword` | `user`, `email`, `passwd`(异或), `varifycode` |

#### HTTP 回包（GateServer → 客户端）

| 场景 | 字段 |
| --- | --- |
| 注册成功 | `error=0`, `email`, `user`, `passwd`, `confirm`, `varifycode` |
| 登录成功 | `error=0`, `uid`, `email`, `token`, `host`, `port` |
| 重置成功 | `error=0` |
| 各种失败 | `error=<错误码>` |

错误码见 `global.h` / `const.h`：`0` 成功，`1001` JSON 解析失败，`1007` 用户名存在，`1008` 密码错误，`1009` 用户不存在……

#### TCP 消息（客户端 ↔ ChatServer）

| 消息 | 字段 |
| --- | --- |
| `MSG_CHAT_LOGIN`（请求） | `uid`, `token` |
| `MSG_CHAT_LOGIN_RSP`（回包） | `error`, `uid`, `user`, `email`, `pwd`, `nick`, `sex`, `icon`, `desc` |

---

### 常见坑汇总

| 坑 | 后果 | 正确做法 |
| --- | --- | --- |
| 客户端缺键直接取值 | `toInt()` 返回 0、`toString()` 返回空串，错误被掩盖 | 先 `contains()` 再取值 |
| jsoncpp `[]` 拼错键名 | 悄悄创建空成员，字段永远为空 | 检查键名 / `isMember()` 判断 |
| 服务端把 `pwd` 放进回包 | 密码（虽然是异或串）发给客户端，泄露面变大 | 回包不包含 `pwd`/`passwd` 这类字段 |
| 注册回包原样回显 `passwd`/`confirm`/`varifycode` | 多余回显，验证码和密码都暴露在响应里 | 只回 `error` + 必要字段 |
| TCP 消息体用 `Indented` | 带宽浪费 | 消息体用 Compact，调试时再看格式化 |
| 中文乱码 | 服务端收不到正确内容 | 客户端 `toUtf8()`，数据库 utf8mb4，HTTP 头 charset 一致 |
| uid 类型不一致 | `asInt` 和 `asString` 混用取错值 | 客户端发数字，服务端 `asInt()`，两端对齐 |
| 解析失败不处理 | 后续 `root["x"]` 全空，行为诡异 | 先判 `parse_success` / `isNull()`，失败回 `ERR_JSON` |
| 忘了 `isObject()` 检查 | 服务器返回数组/字符串时 `.object()` 行为异常 | `isNull() || !isObject()` 双重检查 |

---

### 面试/复习自测

* `QJsonDocument::fromJson` 解析失败会怎样？怎么判断？
* `QJsonValue` 缺键时 `toInt()` / `toString()` 返回什么？为什么说它"安全但不安全"？
* jsoncpp 的 `Value::operator[]` 和 Qt 的 `QJsonObject::operator[]` 在"键不存在"时的行为有什么区别？
* 为什么客户端和 C++ 服务端都用 JSON，而服务间用 protobuf？
* `toJson()` 和 `toJson(QJsonDocument::Indented)` 有什么区别？TCP 消息该用哪个？
* 注册流程里，客户端发的 `passwd` 和数据库存的 `pwd` 是什么关系？
* JSON 协议没有 schema，两端靠什么保证字段一致？
