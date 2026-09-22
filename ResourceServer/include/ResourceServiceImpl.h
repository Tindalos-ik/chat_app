#pragma once

#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"

// gRPC 面向 ChatServer 提供资源可信性边界。它不暴露本机绝对路径，也不接受文件路径。
class ResourceServiceImpl final : public message::ResourceService::Service {
public:
    grpc::Status VerifyImages(grpc::ServerContext* context,
                              const message::ResourceVerifyReq* request,
                              message::ResourceVerifyRsp* response) override;
};
