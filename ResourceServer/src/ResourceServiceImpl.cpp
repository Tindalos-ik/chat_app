#include "ResourceServiceImpl.h"

#include "ResourceVerifier.h"

#include <iostream>
#include <vector>

grpc::Status ResourceServiceImpl::VerifyImages(grpc::ServerContext* context,
                                                const message::ResourceVerifyReq* request,
                                                message::ResourceVerifyRsp* response) {
    (void)context;
    response->set_error(ErrorCodes::Error_Json);
    if (request == nullptr || request->resource_ids_size() <= 0 || request->resource_ids_size() > 100) {
        return grpc::Status::OK;
    }

    // 整批持锁、整批先验证。这样不会出现前半批已确认、后半批仍在上传的“部分成功”状态。
    std::vector<VerifiedImageResource> verified;
    verified.reserve(static_cast<std::size_t>(request->resource_ids_size()));
    std::lock_guard<std::mutex> lock(GetResourceFileMutex());
    for (const std::string& resourceId : request->resource_ids()) {
        VerifiedImageResource resource;
        const ErrorCodes result = ResolveCompletedImageResourceLocked(resourceId, resource);
        if (result != ErrorCodes::Success) {
            // 这条日志直接给出 ChatServer 收到 1034 失败时的根因：资源不存在、
            // 尚未完成，还是文件格式不在当前白名单内。
            std::cout << "verify image resource rejected, resource_id=" << resourceId
                      << ", error=" << result << std::endl;
            response->set_error(result);
            return grpc::Status::OK;
        }
        verified.push_back(std::move(resource));
    }

    response->set_error(ErrorCodes::Success);
    for (const VerifiedImageResource& resource : verified) {
        auto* item = response->add_resources();
        item->set_resource_id(resource.resourceId);
        item->set_name(resource.name);
        item->set_mime_type(resource.mimeType);
        item->set_file_size(resource.fileSize);
        item->set_width(resource.width);
        item->set_height(resource.height);
    }
    return grpc::Status::OK;
}
