// 文件作用：单向预定文件测试信令；仅Redis短期状态，不创建聊天会话或持久消息。
#include "LogicSystem.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include "UserMgr.h"
#include "ConfigMgr.h"
#include "ChatGrpcClient.h"
#include "ChatServiceImpl.h"
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <sstream>

namespace {
// 元数据与双方session级占位固定120秒到期；不续租，旧ready/stop不会释放新测试占位。
const char* kStateScript = R"lua(
local op=ARGV[1]
local function slot(uid,sid) return 'vtest:slot:'..uid..':'..sid end
if op=='create' then
 local m=cjson.decode(ARGV[2])
 if redis.call('GET','usession_'..m.fromuid)~=m.from_session then return 'forbidden' end
 if redis.call('GET','usession_'..m.touid)~=m.to_session then return 'unreachable' end
 local a=slot(m.fromuid,m.from_session); local b=slot(m.touid,m.to_session)
 if redis.call('EXISTS',a)==1 or redis.call('EXISTS',b)==1 then return 'busy' end
 m.state='pending'
 redis.call('SET',KEYS[1],cjson.encode(m),'EX',120)
 redis.call('SET',a,m.test_id,'EX',120); redis.call('SET',b,m.test_id,'EX',120)
 return 'ok'
end
local raw=redis.call('GET',KEYS[1]); if not raw then return 'invalid' end
local m=cjson.decode(raw)
if op=='notified' then
 if m.state~='pending' then return 'invalid' end
 local ttl=redis.call('TTL',KEYS[1]); if ttl<=0 then return 'invalid' end
 m.state='notified'; redis.call('SET',KEYS[1],cjson.encode(m),'EX',ttl); return 'ok'
end
local uid=tonumber(ARGV[2]); local sid=ARGV[3]
local sender=uid==m.fromuid and sid==m.from_session
local receiver=uid==m.touid and sid==m.to_session
if not sender and not receiver then return 'forbidden' end
if op=='ready' then
 if not sender or redis.call('GET','usession_'..uid)~=sid then return 'forbidden' end
 if m.state~='notified' then return 'invalid' end
 local ttl=redis.call('TTL',KEYS[1]); if ttl<=0 then return 'invalid' end
 m.state='ready'; redis.call('SET',KEYS[1],cjson.encode(m),'EX',ttl); return raw
end
if op=='stop' then
 redis.call('DEL',KEYS[1])
 for _,key in ipairs({slot(m.fromuid,m.from_session),slot(m.touid,m.to_session)}) do
  if redis.call('GET',key)==m.test_id then redis.call('DEL',key) end
 end
 return raw
end
return 'invalid'
)lua";

// 使用统一紧凑JSON限制TCP包体；所有媒体参数只取服务端配置及随机元数据。
std::string Pack(const Json::Value& value) {
    Json::StreamWriterBuilder builder; builder["indentation"] = "";
    return Json::writeString(builder, value);
}

// JSON解析同时用于TCP与可信RPC信封；异常/语法错误由业务入口返回invalid。
bool Parse(const std::string& text, Json::Value& value) {
    Json::CharReaderBuilder builder; std::string errors; std::istringstream input(text);
    return Json::parseFromStream(builder, input, &value, &errors) && value.isObject();
}

// UUID固定36字符，限制Redis键与回包；test_id由服务端生成且不接受客户端指定。
bool IsUuid(const std::string& text) {
    if (text.size() != 36) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
        else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                   (text[i] >= 'A' && text[i] <= 'F'))) return false;
    }
    return true;
}

// 状态操作统一区分Redis故障与业务拒绝；result可为状态或完整元数据。
std::string State(const std::string& op, const std::string& id,
                  const std::vector<std::string>& args = {}) {
    std::vector<std::string> params{op}; params.insert(params.end(), args.begin(), args.end());
    std::string result;
    if (!RedisMgr::GetInstance()->VideoTestScript(kStateScript, {"vtest:meta:" + id}, params, result))
        return "rpc_failed";
    return result;
}

// 查询在线路由时区分Redis故障与无key；Get的bool接口无法区分这两种情况。
std::string Lookup(const std::string& key, std::string& value) {
    if (!RedisMgr::GetInstance()->VideoTestScript(
            "local v=redis.call('GET',KEYS[1]); if not v then return '' end; return v", {key}, {}, value))
        return "rpc_failed";
    return value.empty() ? "offline" : "ok";
}

// TCP与RPC使用统一状态码；rpc_failed涵盖Redis/MySQL依赖故障及gRPC传输失败。
int Error(const std::string& status) {
    if (status == "notified" || status == "ready" || status == "stopped") return Success;
    if (status == "forbidden") return VideoTestForbidden;
    if (status == "invalid") return Error_Json;
    if (status == "busy") return ServerBusy;
    if (status == "rpc_failed") return RPCFaild;
    return VideoTestUnavailable;
}

// 仅公开媒体所需字段，不把双方session代号或内部状态交给客户端。
Json::Value Notice(const Json::Value& meta, const std::string& op, const std::string& status) {
    Json::Value result;
    for (const auto* key : {"test_id", "stream_id", "app", "media_url", "fromuid", "touid"})
        result[key] = meta[key];
    result["op"] = op; result["status"] = status; result["error"] = Error(status);
    return result;
}

// 同服和RPC共用当前登录校验，TrySend成功才算通知入队，绝不把陈旧路由算成成功。
std::string DeliverLocal(int uid, const std::string& sid, const std::string& payload) {
    const auto target = UserMgr::GetInstance()->GetSession(uid);
    if (!target || target->GetSessionId() != sid) return "unreachable";
    std::string current;
    const auto lookup = Lookup(USER_SESSION_PREFIX + std::to_string(uid), current);
    if (lookup == "rpc_failed") return lookup;
    if (lookup != "ok" || current != sid) return "unreachable";
    return target->TrySend(payload, ID_NOTIFY_VIDEO_TEST) ? "notified" : "unreachable";
}

// 转发到元数据绑定的那一代连接；新登录不能接到旧测试stop/ready。
std::string Forward(const Json::Value& meta, int uid, const Json::Value& notice) {
    const auto sid = meta[uid == meta["fromuid"].asInt() ? "from_session" : "to_session"].asString();
    std::string server;
    const auto lookup = Lookup(USERIPPREFIX + std::to_string(uid), server);
    if (lookup != "ok") return lookup;
    if (server == ConfigMgr::Inst()["SelfChatServer"]["name"])
        return DeliverLocal(uid, sid, Pack(notice));
    message::VideoTestNotifyReq request;
    request.set_target_uid(uid); request.set_target_session(sid); request.set_payload_json(Pack(notice));
    return ChatGrpcClient::GetInstance()->NotifyVideoTest(server, request).status();
}

// 已授权stop先原子撤销状态，再尽力通知另一方；通知失败不恢复已停止测试。
std::string VideoTestStop(const std::string& id, int uid, const std::string& sid,
                          const std::string& reason, Json::Value& meta, std::string& delivery) {
    const auto result = State("stop", id, {std::to_string(uid), sid});
    if (!Parse(result, meta)) return result;
    auto notice = Notice(meta, "stop", "stopped");
    notice["reason"] = reason; notice["actoruid"] = uid;
    const int peer = uid == meta["fromuid"].asInt() ? meta["touid"].asInt() : meta["fromuid"].asInt();
    delivery = Forward(meta, peer, notice);
    return "stopped";
}
} // namespace

// TCP主入口：start只读好友授权，ready只允许发布者在已通知后发送，双方均可stop。
void LogicSystem::HandleVideoTest(std::shared_ptr<CSession> session, const short&, const std::string& data) {
    Json::Value input, response;
    response["op"] = ""; response["request_id"] = "";
    std::string status = "invalid";
    Defer reply([&]() { response["status"] = status; response["error"] = Error(status);
                       session->TrySend(Pack(response), ID_VIDEO_TEST_RSP); });
    if (!Parse(data, input)) return;
    if (!input["op"].isString() || !input["request_id"].isString()) return;
    const auto op = input["op"].asString(); const auto request = input["request_id"].asString();
    response["op"] = op; if (request.size() <= 36) response["request_id"] = request;
    if (!IsUuid(request)) return;
    const int uid = session->GetUserId();
    const auto current = UserMgr::GetInstance()->GetSession(uid);
    std::string sid;
    if (uid <= 0 || !current || current != session) { status = "forbidden"; return; }
    status = Lookup(USER_SESSION_PREFIX + std::to_string(uid), sid);
    if (status == "rpc_failed") return;
    if (status != "ok" || sid != session->GetSessionId()) { status = "forbidden"; return; }
    status = "invalid";
    if (op == "start") {
        if (!input["touid"].isInt()) return;
        const int peer = input["touid"].asInt();
        const int auth = MysqlMgr::GetInstance()->AuthorizeVideoTest(uid, peer);
        if (auth != Success) { status = auth == RPCFaild ? "rpc_failed" : "forbidden"; return; }
        std::string targetServer;
        status = Lookup(USERIPPREFIX + std::to_string(peer), targetServer);
        if (status != "ok") return; // 路由完全不存在才报告offline。
        std::string targetSid;
        status = Lookup(USER_SESSION_PREFIX + std::to_string(peer), targetSid);
        if (status != "ok") { if (status == "offline") status = "unreachable"; return; }
        const auto enabled = ConfigMgr::Inst()["VideoTest"]["enabled"];
        const auto url = ConfigMgr::Inst()["VideoTest"]["media_url"];
        if ((enabled != "true" && enabled != "1") || url.empty()) { status = "media_unavailable"; return; }
        Json::Value meta;
        const auto id = boost::uuids::to_string(boost::uuids::random_generator()());
        meta["test_id"] = id;
        meta["stream_id"] = boost::uuids::to_string(boost::uuids::random_generator()());
        meta["app"] = "video_test"; meta["media_url"] = url;
        meta["fromuid"] = uid; meta["touid"] = peer;
        meta["from_session"] = sid; meta["to_session"] = targetSid;
        status = State("create", id, {Pack(meta)});
        if (status != "ok") return;
        auto notice = Notice(meta, "start", "notified"); notice["request_id"] = request;
        status = Forward(meta, peer, notice);
        if (status == "notified") {
            const auto marked = State("notified", id);
            if (marked == "ok") { response = notice; return; }
            status = marked;
        }
        // 失败绝不把媒体参数放进成功回包；RPC超时也可能已入队，发送精确stop回收对端窗口。
        Json::Value stopped; std::string ignored;
        VideoTestStop(id, uid, sid, "media_error", stopped, ignored);
        return;
    }
    if (!input["test_id"].isString() || !IsUuid(input["test_id"].asString())) return;
    const auto id = input["test_id"].asString(); response["test_id"] = id;
    if (op == "stop") {
        std::string reason = "closed";
        if (input.isMember("reason")) {
            if (!input["reason"].isString() || input["reason"].asString().size() > 64) return;
            reason = input["reason"].asString();
        }
        Json::Value meta; std::string delivery;
        status = VideoTestStop(id, uid, sid, reason, meta, delivery);
        if (status == "stopped") response["delivery_status"] = delivery;
        return;
    }
    if (op == "ready") {
        Json::Value meta;
        const auto result = State("ready", id, {std::to_string(uid), sid});
        if (!Parse(result, meta)) { status = result; return; }
        auto notice = Notice(meta, "ready", "ready"); notice["request_id"] = request;
        status = Forward(meta, meta["touid"].asInt(), notice);
        if (status == "notified") { status = "ready"; return; }
        Json::Value stopped; std::string ignored;
        VideoTestStop(id, uid, sid, "media_error", stopped, ignored);
    }
}

// 内部断线事件只读取旧session专属占位，不依赖已删除或已切换的在线路由。
void LogicSystem::DisconnectVideoTest(std::shared_ptr<CSession> session) {
    const int uid = session->GetUserId();
    std::string id;
    if (Lookup("vtest:slot:" + std::to_string(uid) + ":" + session->GetSessionId(), id) != "ok") return;
    Json::Value meta; std::string ignored;
    VideoTestStop(id, uid, session->GetSessionId(), "logout", meta, ignored);
}

// 跨服信令池等待及RPC共享三秒绝对截止时间，任何传输失败保留rpc_failed。
message::VideoTestNotifyRsp ChatGrpcClient::NotifyVideoTest(
        const std::string& server, const message::VideoTestNotifyReq& request) {
    message::VideoTestNotifyRsp response;
    response.set_error(RPCFaild); response.set_status("rpc_failed");
    const auto it = _pools.find(server); if (it == _pools.end()) return response;
    auto& pool = it->second;
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
    ClientContext context; context.set_deadline(deadline);
    auto stub = pool->getConnectionUntil(deadline); if (!stub) return response;
    Defer release([&]() { pool->returnConnection(std::move(stub)); });
    if (!stub->NotifyVideoTest(&context, request, &response).ok()) {
        response.set_error(RPCFaild); response.set_status("rpc_failed");
    }
    return response;
}

// RPC仅面向可信ChatServer内网；与本机投递使用相同session精确匹配和TrySend语义。
Status ChatServiceImpl::NotifyVideoTest(ServerContext* context, const message::VideoTestNotifyReq* request,
                                       message::VideoTestNotifyRsp* response) {
    std::string status = "invalid";
    Json::Value notice;
    if (!context->IsCancelled() && request->target_uid() > 0 &&
        !request->target_session().empty() && request->target_session().size() <= 128 &&
        request->payload_json().size() <= 2044 &&
        Parse(request->payload_json(), notice))
        status = DeliverLocal(request->target_uid(), request->target_session(), request->payload_json());
    response->set_status(status); response->set_error(Error(status));
    return Status::OK;
}
