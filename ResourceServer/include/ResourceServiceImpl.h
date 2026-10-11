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
    // VerifyGroupAvatar参数context含配置服务凭据；request含认证UID/token、群号及不透明任务ID。
    // response返回业务错误或可信资源ID/MIME/大小/尺寸（不含路径）；传输Status OK不代表业务成功。
    // 验服务身份→Status当前token→群/成员管理租约→文件绑定/完整解码；调用方发布仍须重查权限。
    grpc::Status VerifyGroupAvatar(grpc::ServerContext* context,
        const message::GroupAvatarVerifyReq* request,
        message::GroupAvatarVerifyRsp* response) override;
};
