#pragma once

#include "singleton.h"
#include "ConfigMgr.h"
#include "const.h"
#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include <chrono>
#include <iostream>
#include <memory>

// 文件作用：封装 ChatServer 到 ResourceServer 的 gRPC 客户端调用。
// VerifyImages/VerifyFiles 只返回资源服务认定的可信元数据；调用方不得把客户端属性落库。
class ResourceGrpcClient : public Singleton<ResourceGrpcClient> {
    friend class Singleton<ResourceGrpcClient>;
public:
    // 核验已发布图片资源。request 为待核验资源 ID 列表，响应含权威图片元数据。
    message::ResourceVerifyRsp VerifyImages(const message::ResourceVerifyReq& request);
    // 核验已发布普通文件。request 为待核验资源 ID 列表，响应含权威名称/MIME/字节数。
    message::ResourceFileVerifyRsp VerifyFiles(const message::ResourceFileVerifyReq& request);

private:
    ResourceGrpcClient();
    std::unique_ptr<message::ResourceService::Stub> stub_;
};

// 保持实现为头文件内联，使 sync_chatservers.ps1 只复制 include/src 时，ChatServer2
// 无须改动其独立 CMake 源文件清单也能获得同一份 gRPC 客户端实现。
inline ResourceGrpcClient::ResourceGrpcClient() {
    // SectionInfo 目前仅提供非 const 的 operator[]；配置是值拷贝，读取时不影响
    // ConfigMgr 内部数据，因此不要把该临时 section 声明为 const。
    auto section = ConfigMgr::Inst()["ResourceServer"];
    const std::string host = section["host"].empty() ? "127.0.0.1" : section["host"];
    const std::string port = section["rpcport"].empty() ? "50057" : section["rpcport"];
    auto channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
    stub_ = message::ResourceService::NewStub(channel);
}

inline message::ResourceVerifyRsp ResourceGrpcClient::VerifyImages(
    const message::ResourceVerifyReq& request) {
    message::ResourceVerifyRsp response;
    response.set_error(ErrorCode::RPCFaild);
    if (!stub_) {
        return response;
    }
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    const grpc::Status status = stub_->VerifyImages(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "verify image resources RPC failed, code=" << status.error_code()
                  << ", message=" << status.error_message() << std::endl;
        response.Clear();
        response.set_error(ErrorCode::RPCFaild);
    }
    return response;
}

inline message::ResourceFileVerifyRsp ResourceGrpcClient::VerifyFiles(
    const message::ResourceFileVerifyReq& request) {
    message::ResourceFileVerifyRsp response;
    response.set_error(ErrorCode::RPCFaild);
    if (!stub_) return response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    const grpc::Status status = stub_->VerifyFiles(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "verify file resources RPC failed, code=" << status.error_code()
                  << ", message=" << status.error_message() << std::endl;
        response.Clear();
        response.set_error(ErrorCode::RPCFaild);
    }
    return response;
}
