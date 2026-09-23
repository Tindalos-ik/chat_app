#pragma once

// 文件作用：实现 ResourceService gRPC 接口，为 ChatServer 核验已完成的图片与普通文件资源。
#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"

// ResourceServer 到 ChatServer 的资源可信性边界：只返回核验后的业务元数据，绝不暴露本机路径。
class ResourceServiceImpl final : public message::ResourceService::Service {
public:
    // 原子核验图片批次。参数分别为 RPC 上下文、资源 ID 列表请求、写入错误码及已核验资源的响应。
    grpc::Status VerifyImages(grpc::ServerContext* context,
                              const message::ResourceVerifyReq* request,
                              message::ResourceVerifyRsp* response) override;

    // 原子核验普通文件批次；任意资源失败时不返回部分资源元数据。
    // 参数分别为 RPC 上下文、ResourceFileVerifyReq、写入错误码及可信文件元数据的响应。
    grpc::Status VerifyFiles(grpc::ServerContext* context,
                             const message::ResourceFileVerifyReq* request,
                             message::ResourceFileVerifyRsp* response) override;
};
