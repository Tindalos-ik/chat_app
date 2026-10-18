// 文件职责：视频test与好友语音共用JSON、UUID、Redis路由和精确session投递，避免两套信令基础设施。
#pragma once // 同一翻译单元只定义一次内联工具。
#include "RedisMgr.h" // 复用现有有界Redis脚本接口。
#include "UserMgr.h" // 查询目标当前连接。
#include "CSession.h" // 精确投递需要完整会话类型及统一协议常量。
#include <json.h> // 明确声明JSON工具依赖，头文件可独立包含。
#include <sstream> // 从JSON文本构造解析流。

namespace MediaSignal { // 工具无通话成员状态，所有身份由调用者显式传入。
// value为公开信令对象；返回紧凑JSON，减少TCP包体占用。
inline std::string Pack(const Json::Value& value) { // 序列化供TCP和RPC共用。
    Json::StreamWriterBuilder builder; // 写入配置仅属于本次调用。
    builder["indentation"] = ""; // 禁用格式缩进。
    return Json::writeString(builder, value); // 返回拥有存储的JSON文本。
}
// text为输入文本，value为解析输出；仅对象成功返回true。
inline bool Parse(const std::string& text, Json::Value& value) { // TCP与可信RPC采用相同对象约束。
    Json::CharReaderBuilder builder; // 每次调用独立解析配置。
    std::string errors; // 接收解析器语法诊断。
    std::istringstream input(text); // 保持现有jsoncpp流式解析约定。
    return Json::parseFromStream(builder, input, &value, &errors) && value.isObject(); // 拒绝标量和数组。
}
// text为请求关联或服务端生成的UUID；返回是否为标准36字符十六进制形式。
inline bool IsUuid(const std::string& text) { // 限制Redis键和响应字段长度。
    if (text.size() != 36) return false; // 固定UUID文本长度。
    for (size_t i = 0; i < text.size(); ++i) { // 按标准分隔位置检查。
        if (i == 8 || i == 13 || i == 18 || i == 23) { // 四处固定连字符。
            if (text[i] != '-') return false; // 拒绝错误分隔符。
        } else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                     (text[i] >= 'A' && text[i] <= 'F'))) return false; // 其余位置只接受十六进制。
    }
    return true; // 全部位置满足UUID形状。
}
// key为Redis路由或占位键，value接收其值；返回ok/offline/rpc_failed，故障不冒充离线。
inline std::string Lookup(const std::string& key, std::string& value) { // GET保持现有脚本连接池与截止行为。
    if (!RedisMgr::GetInstance()->VideoTestScript( // 复用现有通用EVAL入口，无额外Redis连接。
            "local v=redis.call('GET',KEYS[1]); if not v then return '' end; return v", {key}, {}, value))
        return "rpc_failed"; // 区分依赖失败。
    return value.empty() ? "offline" : "ok"; // 缺失键才报告离线。
}
// uid/sid绑定目标那一代连接，payload为紧凑JSON，id为通知号；返回notified/unreachable/rpc_failed。
inline std::string DeliverCurrent(int uid, const std::string& sid, const std::string& payload, short id) {
    const auto target = UserMgr::GetInstance()->GetSession(uid); // 固定本次投递使用的连接实例。
    if (!target || target->GetSessionId() != sid) return "unreachable"; // 旧信令绝不进入新连接。
    std::string current; // 接收Redis当前登录代号。
    const auto lookup = Lookup(USER_SESSION_PREFIX + std::to_string(uid), current); // 再核对分布式登录身份。
    if (lookup == "rpc_failed") return lookup; // 依赖故障保持原始语义。
    if (lookup != "ok" || current != sid) return "unreachable"; // 拒绝过期本机路由。
    return target->TrySend(payload, id) ? "notified" : "unreachable"; // 只有实际入队才视为通知成功。
}
} // namespace MediaSignal
