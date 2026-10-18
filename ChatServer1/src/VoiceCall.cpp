// 文件职责：好友双向语音控制面，复用媒体配置与现有RPC池；不承载音频、不创建聊天消息。
#include "LogicSystem.h" // TCP业务与断线入口归属LogicSystem。
#include "MediaSignal.h" // 复用视频test的JSON、UUID、路由与精确投递。
#include "MysqlMgr.h" // 复用只读双向好友授权。
#include "ConfigMgr.h" // 复用VideoTest媒体配置。
#include "ChatGrpcClient.h" // 复用跨服连接池。
#include "ChatServiceImpl.h" // 实现专用语音RPC接收。
#include <boost/uuid/random_generator.hpp> // 服务端生成不可猜测的通话及双流标识。
#include <boost/uuid/uuid_io.hpp> // UUID转标准文本。
#include <mutex> // 将同目标终态通知与活跃状态检查、入队排序。

namespace { // 私有状态机和投递工具只属于语音翻译单元。
using MediaSignal::Pack; // 使用统一紧凑JSON。
using MediaSignal::Parse; // 使用统一对象解析。
using MediaSignal::IsUuid; // 使用统一UUID格式。
using MediaSignal::Lookup; // 使用区分Redis故障的路由查询。
std::mutex kDeliveryMutex; // 本服语音入队阶段的锁，避免ended之后再入队迟到accepted/ready。
// Redis原子管理元数据和双方UID占位；邀请120秒，接听后PERSIST直到挂断/断线，活跃通话不会固定到期。
const char* kVoiceScript = R"lua(
local op=ARGV[1] -- 第一参数选择状态操作。
local function slot(uid) return 'voice:slot:'..uid end -- 每个用户仅占用一路语音。
local function remove(m) -- 删除元数据时只释放仍属于该call_id的占位。
 redis.call('DEL','voice:meta:'..m.call_id) -- 删除通话状态，迟到操作不得恢复。
 for _,uid in ipairs({m.fromuid,m.touid}) do -- 同一原子操作中处理双方占位。
  local key=slot(uid) -- 找到该用户当前占位。
  if redis.call('GET',key)==m.call_id then redis.call('DEL',key) end -- 保留已经替换的新通话。
 end
end
local function current(m) -- 两个精确登录仍存活才允许继续活动操作。
 return redis.call('GET','usession_'..m.fromuid)==m.from_session and redis.call('GET','usession_'..m.touid)==m.to_session -- 防止新登录继承旧通话。
end
if op=='create' then -- 原子验证身份并占用双方slot。
 local m=cjson.decode(ARGV[2]) -- 元数据完全由服务端组装。
 if redis.call('GET','usession_'..m.fromuid)~=m.from_session then return 'forbidden' end -- 调用者必须仍为当前登录。
 if redis.call('GET','usession_'..m.touid)~=m.to_session then return 'unreachable' end -- 目标登录必须与查到的路由一致。
 for _,uid in ipairs({m.fromuid,m.touid}) do -- 创建前顺手回收崩溃遗留的旧登录通话。
  local id=redis.call('GET',slot(uid)) -- 读取已占用的call_id。
  if id then -- 仅检查确实存在的旧占位。
   local raw=redis.call('GET','voice:meta:'..id) -- 旧元数据可能随邀请超时消失。
   if not raw then redis.call('DEL',slot(uid)) -- 回收孤立占位。
   else -- 有元数据时必须先证明旧登录已经失效。
    local old=cjson.decode(raw) -- 解码旧通话精确session。
    if current(old) then return 'busy' end -- 活跃通话始终保持占位，不设置固定通话时长。
    remove(old) -- 登录换代后回收旧通话与双方旧占位。
   end
  end
 end
 m.state='invited' -- 接听只能从邀请态原子转换。
 redis.call('SET',KEYS[1],cjson.encode(m),'EX',120) -- 等待接听设置有限生命周期。
 redis.call('SET',slot(m.fromuid),m.call_id,'EX',120) -- 主叫等待占位与邀请同寿命。
 redis.call('SET',slot(m.touid),m.call_id,'EX',120) -- 被叫等待占位与邀请同寿命。
 return cjson.encode(m) -- 返回实际提交的服务端状态。
end
local raw=redis.call('GET',KEYS[1]) -- 其他操作必须引用现有call_id。
if not raw then return 'invalid' end -- 已结束、超时或未知通话无法复活。
local m=cjson.decode(raw) -- 后续身份校验使用服务端绑定信息。
local uid=tonumber(ARGV[2]) -- 请求身份来自认证session或可信投递目标。
local sid=ARGV[3] -- 精确连接代号禁止跨登录复用。
local caller=uid==m.fromuid and sid==m.from_session -- 判断主叫那一代连接。
local callee=uid==m.touid and sid==m.to_session -- 判断被叫那一代连接。
if not caller and not callee then return 'forbidden' end -- 非参与者不能访问或清理状态。
if op=='abort' or op=='disconnect' then -- 内部失败和断线允许已离线旧session清理。
 remove(m) -- 先撤销后通知，RPC迟到不得恢复。
 return raw -- 返回撤销前元数据用于精确终态通知。
end
if not current(m) then return 'unreachable' end -- 业务变更和活跃投递要求双方仍为原登录。
if op=='deliver' or op=='response' then -- 通知与响应都检查存活，快速接听后的邀请响应允许保留原操作结果。
 local status=ARGV[4] -- 通知或响应由可信ChatServer提供状态名称。
 if status=='invited' and m.state~='invited' and not (op=='response' and m.state=='accepted') then return 'invalid' end -- 迟到邀请通知丢弃，原邀请响应不能将快速接听误报失败。
 if (status=='accepted' or status=='ready') and m.state~='accepted' then return 'invalid' end -- 终态以后丢弃迟到媒体参数。
 if status=='ready' then -- ready必须确实经过发布者原子提交。
  local actor=tonumber(ARGV[5]) -- 通知携带实际发布者。
  if (actor~=m.fromuid and actor~=m.touid) or not m['ready_'..actor] then return 'invalid' end -- 拒绝未提交的就绪事件。
 end
 return raw -- 调用端在同一投递锁中接着TrySend。
end
if op=='accept' or op=='reject' then -- 只有被叫可以决定邀请。
 if not callee then return 'forbidden' end -- 主叫不能自接听或伪造拒绝。
 if m.state~='invited' then return 'invalid' end -- 重复或迟到接听不修改状态。
 if op=='reject' then remove(m); return raw end -- 拒绝先原子释放双方占位。
 m.state='accepted' -- 在发送accepted前提交，允许跨服对端立即发布ready。
 redis.call('SET',KEYS[1],cjson.encode(m)) -- 活跃通话无固定TTL，靠精确断线/挂断结束。
 redis.call('PERSIST',slot(m.fromuid)) -- 主叫占位持续到通话结束。
 redis.call('PERSIST',slot(m.touid)) -- 被叫占位持续到通话结束。
 return cjson.encode(m) -- accepted通知使用同一组服务端流信息。
end
if op=='ready' then -- 双方分别确认本端已有真实出站音频。
 if m.state~='accepted' then return 'invalid' end -- 邀请阶段不能ready。
 if m['ready_'..uid] then return 'invalid' end -- 每位参与者只转发一次ready，不触发重复拉流。
 m['ready_'..uid]=true -- 原子锁存发布者就绪事实。
 redis.call('SET',KEYS[1],cjson.encode(m)) -- 保持活跃元数据无TTL。
 return cjson.encode(m) -- 返回含就绪标记的已提交快照。
end
if op=='end' then -- 等待阶段仅主叫可取消，通话阶段双方均可挂断。
 if m.state=='invited' and not caller then return 'forbidden' end -- 被叫等待阶段使用reject。
 remove(m) -- 一次原子操作释放双方slot与元数据。
 return raw -- 后续终态通知不需要恢复状态。
end
return 'invalid' -- 未知操作不改变Redis。
)lua";

// op/id为状态动作及服务端call_id，args为精确身份；返回元数据JSON、业务状态或rpc_failed。
std::string State(const std::string& op, const std::string& id, const std::vector<std::string>& args) {
    std::vector<std::string> params{op}; // 第一参数保留给Lua操作选择。
    params.insert(params.end(), args.begin(), args.end()); // 后续参数保留原始长度传入Redis。
    std::string result; // 接收脚本结果。
    if (!RedisMgr::GetInstance()->VideoTestScript(kVoiceScript, {"voice:meta:" + id}, params, result))
        return "rpc_failed"; // 依赖故障不混为业务拒绝。
    return result; // 返回状态或已提交元数据。
}
// status为语音状态；返回现有统一错误码，保留视频test原协议兼容。
int Error(const std::string& status) {
    if (status == "invited" || status == "accepted" || status == "rejected" || status == "ready" || status == "ended" || status == "notified") return Success; // 业务成功状态。
    if (status == "forbidden") return VideoTestForbidden; // 复用现有媒体权限错误码。
    if (status == "invalid") return Error_Json; // 非法格式、阶段或未知call_id。
    if (status == "busy") return ServerBusy; // 任意一方已有通话。
    if (status == "rpc_failed") return RPCFaild; // 保持依赖或传输故障。
    return VideoTestUnavailable; // 离线、不可达或媒体配置关闭。
}
// meta为内部快照，op/status为公开事件；仅accepted公开双向媒体参数，绝不泄露session。
Json::Value Notice(const Json::Value& meta, const std::string& op, const std::string& status) {
    Json::Value notice; // 本次公开通知对象。
    for (const auto* key : {"call_id", "invite_request_id", "fromuid", "touid"}) notice[key] = meta[key]; // 每个状态保留原邀请UUID，隔离同好友连续呼叫的迟到事件。
    if (status == "accepted") { // 只允许双方接听成功后开启采集。
        for (const auto* key : {"app", "media_url", "caller_stream", "callee_stream"}) notice[key] = meta[key]; // 媒体参数全部取服务端元数据。
    }
    notice["op"] = op; // 事件动作与请求一致。
    notice["status"] = status; // 客户端状态机依据状态处理。
    notice["error"] = Error(status); // 与TCP响应共用错误语义。
    return notice; // 返回独立公开对象。
}
// uid/sid为精确目标，payload及notice为同一通知；本机和RPC均在此完成阶段检查与TCP入队。
std::string Deliver(int uid, const std::string& sid, const std::string& payload, const Json::Value& notice) {
    std::lock_guard<std::mutex> guard(kDeliveryMutex); // 终态通知不能被先检查后入队的迟到媒体事件反超。
    const auto status = notice["status"].asString(); // 判定是否需要活跃状态保护。
    if (status == "invited" || status == "accepted" || status == "ready") { // 终态无需读取已删除元数据。
        Json::Value meta; // 接收Redis当前状态而非发送方旧快照。
        const auto state = State("deliver", notice["call_id"].asString(), {std::to_string(uid), sid, status, std::to_string(notice.get("actoruid", 0).asInt())}); // 原子核验当前阶段。
        if (!Parse(state, meta)) return state; // 拒绝迟到、失效登录或依赖故障。
    }
    return MediaSignal::DeliverCurrent(uid, sid, payload, ID_NOTIFY_VOICE_CALL); // TrySend成功才确认通知入队。
}
// meta绑定原登录，uid为实际接收者，notice为公开通知；返回notified或明确失败原因。
std::string Forward(const Json::Value& meta, int uid, const Json::Value& notice) {
    const auto sid = meta[uid == meta["fromuid"].asInt() ? "from_session" : "to_session"].asString(); // 精确路由到通话原session。
    std::string server; // 接收Redis路由名。
    const auto lookup = Lookup(USERIPPREFIX + std::to_string(uid), server); // 每次投递读取当前路由。
    if (lookup != "ok") return lookup == "offline" ? "unreachable" : lookup; // 已建立通话路由消失属于不可达。
    const auto payload = Pack(notice); // 同服与跨服使用相同公开JSON。
    if (server == ConfigMgr::Inst()["SelfChatServer"]["name"]) return Deliver(uid, sid, payload, notice); // 本机直接核验入队。
    message::VoiceCallNotifyReq request; // 使用语音专用RPC信封。
    request.set_target_uid(uid); // 显式指定通知接收者。
    request.set_target_session(sid); // 禁止旧通话通知新连接。
    request.set_payload_json(payload); // 不携带内部session元数据。
    return ChatGrpcClient::GetInstance()->NotifyVoiceCall(server, request).status(); // 保留RPC故障而非伪装offline。
}
// meta为已撤销快照，actor为导致结束者，reason为真实原因；双方尽力收尾，任何迟到成功都不能恢复。
void BroadcastEnd(const Json::Value& meta, int actor, const std::string& reason) {
    auto notice = Notice(meta, "end", "ended"); // 终态通知不含可启动媒体的参数。
    notice["actoruid"] = actor; // 对方可区分谁结束了通话。
    notice["reason"] = reason; // 保留logout、媒体失败或具体投递失败。
    Forward(meta, meta["fromuid"].asInt(), notice); // 主叫收到撤销结果，失败不恢复状态。
    Forward(meta, meta["touid"].asInt(), notice); // 被叫收到撤销结果，含RPC超时后可能已入队的窗口。
}
// id/uid/sid为本次已授权通话；失败先原子撤销，再向双方发送终态。
void Abort(const std::string& id, int uid, const std::string& sid, const std::string& reason) {
    Json::Value meta; // 接收成功撤销的元数据。
    if (Parse(State("abort", id, {std::to_string(uid), sid}), meta)) BroadcastEnd(meta, uid, reason); // 状态已经被其他终态删除时不复活也不重复覆盖。
}
} // namespace

// TCP入口：身份来自session；每次响应回显op/request_id/call_id，只有accepted响应携带媒体参数。
void LogicSystem::HandleVoiceCall(std::shared_ptr<CSession> session, const short&, const std::string& data) {
    Json::Value input, response; // 输入对象与独立响应对象归属本次请求。
    response["op"] = ""; // 非法JSON也保持响应字段形状。
    response["request_id"] = ""; // 无可验证关联号时返回空值。
    response["call_id"] = ""; // invite成功前没有客户端可指定的通话号。
    std::string status = "invalid"; // 所有早退默认非法请求。
    Defer reply([&]() { // 无论哪个业务分支退出都发送一次关联响应。
        std::unique_lock<std::mutex> guard(kDeliveryMutex); // 防止ended入队后再发送迟到accepted成功响应。
        if (status == "invited" || status == "accepted" || status == "ready") { // 返回成功前再次检查通话仍处于有效阶段。
            Json::Value live; // 接收当前元数据用于确认响应没有过时。
            const auto checked = State("response", response["call_id"].asString(), {std::to_string(session->GetUserId()), session->GetSessionId(), status, std::to_string(session->GetUserId())}); // ready响应检查本端ready，快速接听仍保留invite原成功结果。
            if (!Parse(checked, live)) { // 期间对方挂断或投递失败不能再启动媒体。
                status = checked; // 返回真实当前失败状态。
                for (const auto* key : {"app", "media_url", "caller_stream", "callee_stream"}) response.removeMember(key); // 失效响应清除媒体参数。
                guard.unlock(); // 先退出本机入队临界区再转发失败终态。
                Abort(response["call_id"].asString(), session->GetUserId(), session->GetSessionId(), status); // 重新校验失败也不能遗留已接听占位。
                guard.lock(); // 终态尝试结束后继续保证本次响应入队顺序。
            }
        }
        response["status"] = status; // 填入业务最终状态。
        response["error"] = Error(status); // 填入项目统一错误码。
        if (!session->TrySend(Pack(response), ID_VOICE_CALL_RSP) && IsUuid(response["call_id"].asString())) { // 成功操作的响应也必须真实入队。
            guard.unlock(); // 网络转发前释放本服入队锁，避免双方RPC互相持锁等待。
            Abort(response["call_id"].asString(), session->GetUserId(), session->GetSessionId(), "unreachable"); // 响应拒绝入队也撤销并向双方尽力终结。
        }
    });
    if (!Parse(data, input) || !input["op"].isString() || !input["request_id"].isString()) return; // 仅接受对象和字符串关联字段。
    const auto op = input["op"].asString(); // 保存动作以便所有结果回显。
    const auto request = input["request_id"].asString(); // 保存客户端UUID。
    response["op"] = op; // 即使未知动作仍回显原动作。
    if (request.size() <= 36) response["request_id"] = request; // 拒绝超长关联号膨胀响应。
    if (input["call_id"].isString() && input["call_id"].asString().size() <= 36) response["call_id"] = input["call_id"]; // 非invite请求尽早回显通话号。
    if (!IsUuid(request)) return; // 关联号必须为UUID。
    const int uid = session->GetUserId(); // 唯一用户身份来源是已登录连接。
    const auto current = UserMgr::GetInstance()->GetSession(uid); // 检查本机当前连接实例。
    const auto sid = session->GetSessionId(); // 绑定调用者精确登录代号。
    if (uid <= 0 || current != session) { status = "forbidden"; return; } // 未登录或被替换连接不能操作。
    std::string storedSid; // 接收分布式登录记录。
    status = Lookup(USER_SESSION_PREFIX + std::to_string(uid), storedSid); // 与Redis当前登录保持一致。
    if (status == "rpc_failed") return; // 保留Redis故障。
    if (status != "ok" || storedSid != sid) { status = "forbidden"; return; } // 拒绝旧登录重放。
    status = "invalid"; // 后续格式检查默认非法。
    if (op == "invite") { // 主叫不能指定call_id或媒体流。
        response["call_id"] = ""; // invite只回显服务端确认的标识。
        if (!input["touid"].isInt()) return; // 目标必须是整数UID。
        const int peer = input["touid"].asInt(); // 目标不作为请求者身份使用。
        const int auth = MysqlMgr::GetInstance()->AuthorizeVideoTest(uid, peer); // 复用双向好友只读授权，不创建聊天会话。
        if (auth != Success) { status = auth == RPCFaild ? "rpc_failed" : "forbidden"; return; } // 依赖故障与非好友分开。
        std::string server, peerSid; // 接收目标当前路由与精确登录。
        status = Lookup(USERIPPREFIX + std::to_string(peer), server); // 路由键确实不存在才返回offline。
        if (status != "ok") return; // 目标离线或路由依赖失败。
        status = Lookup(USER_SESSION_PREFIX + std::to_string(peer), peerSid); // 读取目标当前登录代号。
        if (status != "ok") { if (status == "offline") status = "unreachable"; return; } // 半残路由是不可达。
        const auto enabled = ConfigMgr::Inst()["VideoTest"]["enabled"]; // 不引入新配置或媒体服务。
        const auto url = ConfigMgr::Inst()["VideoTest"]["media_url"]; // 媒体URL沿用已有ZLM地址。
        if ((enabled != "true" && enabled != "1") || url.empty()) { status = "media_unavailable"; return; } // 媒体不可用不能发出邀请。
        Json::Value meta; // 完整内部状态只保存在Redis。
        const auto id = boost::uuids::to_string(boost::uuids::random_generator()()); // 通话号由服务端确认。
        meta["call_id"] = id; // 对外稳定关联标识。
        meta["invite_request_id"] = request; // 固定保存最初invite的UUID，后续accept/ready/end不能替换这一跨操作关联。
        meta["fromuid"] = uid; // 主叫身份来自session。
        meta["touid"] = peer; // 被叫为已授权好友。
        meta["from_session"] = sid; // 绑定主叫当前连接。
        meta["to_session"] = peerSid; // 绑定被叫当前连接。
        meta["app"] = "voice_call"; // 与保留的视频test使用独立媒体应用。
        meta["media_url"] = url; // 双方使用同一现有ZLM地址。
        meta["caller_stream"] = boost::uuids::to_string(boost::uuids::random_generator()()); // 主叫音频流随机且仅接听后公开。
        meta["callee_stream"] = boost::uuids::to_string(boost::uuids::random_generator()()); // 被叫音频使用独立随机流。
        Json::Value created; // 接收Redis提交后的元数据。
        const auto result = State("create", id, {Pack(meta)}); // 原子占用双方slot并校验两代session。
        if (!Parse(result, created)) { status = result; return; } // busy等失败不发送邀请。
        response["call_id"] = id; // 已提交call_id无论后续投递是否成功都可关联。
        auto notice = Notice(created, op, "invited"); // 只公开邀请身份，不启动采集。
        notice["request_id"] = request; // 对方和响应共用原请求关联号。
        status = Forward(created, peer, notice); // 只有目标TrySend成功才返回invited。
        if (status != "notified") { Abort(id, uid, sid, status); return; } // 任何不确定结果均撤销并向双方收尾。
        response = notice; // 成功响应包含服务端通话号及双方UID。
        status = "invited"; // 客户端保持等待接听。
        return; // 统一响应出口检查状态仍有效。
    }
    if (!input["call_id"].isString() || !IsUuid(input["call_id"].asString())) return; // 其他操作必须引用服务端UUID。
    const auto id = input["call_id"].asString(); // 固定本次操作通话号。
    if (op != "accept" && op != "reject" && op != "ready" && op != "end") return; // 未知动作不进入状态机。
    std::string reason = "closed"; // 普通挂断的默认原因。
    if (op == "end" && input.isMember("reason")) { // 可选结束原因长度受限。
        if (!input["reason"].isString() || input["reason"].asString().size() > 64) return; // 不把任意JSON或大文本转发给对方。
        reason = input["reason"].asString(); // 保留客户端实际媒体错误原因。
    }
    Json::Value meta; // 接收原子状态转换后的快照。
    const auto result = State(op, id, {std::to_string(uid), sid}); // Lua实施角色、阶段与精确session授权。
    if (!Parse(result, meta)) { // 拒绝结果不携带内部元数据。
        status = result; // 保留forbidden/invalid/unreachable/rpc_failed。
        if (status == "unreachable") Abort(id, uid, sid, status); // 对方登录已失效时立即回收本次通话。
        return; // 统一回包。
    }
    status = op == "accept" ? "accepted" : op == "reject" ? "rejected" : op == "end" ? "ended" : "ready"; // 操作映射共享协议状态。
    auto notice = Notice(meta, op, status); // accepted才公开双向推拉参数。
    notice["request_id"] = request; // 保持关联号。
    if (op == "ready" || op == "end") notice["actoruid"] = uid; // 标明发布完成者或挂断者。
    if (op == "end") notice["reason"] = reason; // 结束事件携带明确原因。
    const int peer = uid == meta["fromuid"].asInt() ? meta["touid"].asInt() : meta["fromuid"].asInt(); // 只转发给另一位参与者。
    const auto delivered = Forward(meta, peer, notice); // 接听通知可能与跨服ready并行，目标页面负责锁存提前ready。
    if (delivered != "notified") { // 转发失败不保留可用通话状态。
        if (op == "end" || op == "reject") BroadcastEnd(meta, uid, delivered); // 状态已撤销，仍向双方尽力补终态。
        else Abort(id, uid, sid, delivered); // 活跃状态先原子撤销再通知双方。
        status = delivered; // 不将RPC超时伪装成接听或ready成功。
        return; // 失败响应不公开媒体参数。
    }
    response = notice; // 本端得到与对端一致的最终公开结果。
}

// 内部断线入口只读UID占位，再由Lua核对旧session；新登录的通话不受旧连接清理影响。
void LogicSystem::DisconnectVoiceCall(std::shared_ptr<CSession> session) {
    const int uid = session->GetUserId(); // 断线事件保留原登录UID。
    if (uid <= 0) return; // 未登录连接没有语音占位。
    std::string id; // 接收该UID当前占位的call_id。
    if (Lookup("voice:slot:" + std::to_string(uid), id) != "ok") return; // 无占位或依赖失败时不猜测目标。
    Json::Value meta; // 接收仅旧session获准撤销的快照。
    if (Parse(State("disconnect", id, {std::to_string(uid), session->GetSessionId()}), meta)) BroadcastEnd(meta, uid, "logout"); // 新session代号不匹配便不会删除。
}

// 跨服语音使用现有连接池；池获取和RPC传输共享三秒绝对deadline，不额外重试。
message::VoiceCallNotifyRsp ChatGrpcClient::NotifyVoiceCall(const std::string& server, const message::VoiceCallNotifyReq& request) {
    message::VoiceCallNotifyRsp response; // 默认传输失败，只有真实RPC结果可覆盖。
    response.set_error(RPCFaild); // 保留现有项目错误码。
    response.set_status("rpc_failed"); // 明确区别目标离线。
    const auto it = _pools.find(server); // 复用已配置ChatServer池。
    if (it == _pools.end()) return response; // 未配置路由属于RPC故障。
    auto& pool = it->second; // 获取该目标服务器池。
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3); // 排队与传输共用预算。
    ClientContext context; // 请求独立上下文。
    context.set_deadline(deadline); // 不允许无限阻塞逻辑线程。
    auto stub = pool->getConnectionUntil(deadline); // 有界等待现有RPC连接。
    if (!stub) return response; // 池耗尽保留rpc_failed。
    Defer release([&]() { pool->returnConnection(std::move(stub)); }); // 各出口归还池连接。
    if (!stub->NotifyVoiceCall(&context, request, &response).ok()) { // 传输或截止失败即使对方可能已入队也不算成功。
        response.set_error(RPCFaild); // 统一传输故障错误码。
        response.set_status("rpc_failed"); // 调用方将撤销状态并发送终态。
    }
    return response; // 返回目标TrySend或明确故障结果。
}

// 内网RPC接收也核对精确登录和Redis阶段，迟到accepted/ready不能在ended之后启动媒体。
Status ChatServiceImpl::NotifyVoiceCall(ServerContext* context, const message::VoiceCallNotifyReq* request, message::VoiceCallNotifyRsp* response) {
    std::string status = "invalid"; // 无效信封默认拒绝。
    Json::Value notice; // 解析后的公开通知。
    if (!context->IsCancelled() && request->target_uid() > 0 && !request->target_session().empty() && request->target_session().size() <= 128 && request->payload_json().size() <= 2044 && Parse(request->payload_json(), notice) && notice["call_id"].isString() && IsUuid(notice["call_id"].asString()) && notice["status"].isString()) { // 与现有TCP包体和session长度边界一致。
        const auto event = notice["status"].asString(); // 限定可公开的语音通知状态。
        if (event == "invited" || event == "accepted" || event == "rejected" || event == "ready" || event == "ended") status = Deliver(request->target_uid(), request->target_session(), request->payload_json(), notice); // 活跃通知入队前再次检查状态。
    }
    response->set_status(status); // 返回精确业务或入队结果。
    response->set_error(Error(status)); // 返回现有统一错误码。
    return Status::OK; // 业务失败用响应表达，传输层保持成功。
}
