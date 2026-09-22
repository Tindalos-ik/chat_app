#pragma once //防止重复包含
#ifndef _CONST_H_
#define _CONST_H_

#include <memory>
#include <functional>

enum ErrorCodes {
    Success = 0,
    Error_Json = 1001,  //Json解析错误
    FileNotExists = 1012,
    UploadTaskConflict = 1013, // upload_id 对应的任务元数据不匹配
    UploadOffsetMismatch = 1014, // 分片偏移量和服务端已确认进度不一致
    UploadFileError = 1015, // 创建、写入或发布上传文件失败
    ResourceNotFound = 1016, // resource_id 不存在，或其文件已丢失
    ResourceNotCompleted = 1017, // 资源仍在上传，不能读取半成品
    DownloadFileError = 1018, // 打开、定位或读取已发布资源失败
    ResourceNotImage = 1019 // 已完成资源不是可识别的 PNG/JPEG/GIF/WebP 图片
};


// Defer类
class Defer {
public:
    // 接受一个lambda表达式或者函数指针
    Defer(std::function<void()> func) : func_(func) {}

    // 析构函数中执行传入的函数
    ~Defer() {
        func_();
    }

private:
    std::function<void()> func_;
};

#define MAX_LENGTH  1024*4
//头部总长度
#define HEAD_TOTAL_LEN 6
//头部id长度1
#define HEAD_ID_LEN 2
//头部数据长度
#define HEAD_DATA_LEN 4
// 接受队列最大个数
#define MAX_RECVQUE  2000000
// 发送队列最大个数
#define MAX_SENDQUE 2000000


enum ReqId {
    ID_TEST_MSG_REQ = 1001,       //测试消息
    ID_TEST_MSG_RSP = 1002,       //测试消息回包
    ID_UPLOAD_FILE_REQ = 1003,    //发送文件请求
    ID_UPLOAD_FILE_RSP = 1004,    //发送文件回复
    ID_SYNC_FILE_REQ = 1005,      //同步文件信息请求
    ID_SYNC_FILE_RSP = 1006,      //同步文件回复
    // 下载采用“客户端请求一个分片，服务端回复一个分片”的拉取式协议。
    // 资源标识必须是已完成上传任务的 upload_id，而不是服务器本机文件路径。
    ID_DOWNLOAD_FILE_REQ = 1007,
    ID_DOWNLOAD_FILE_RSP = 1008,
};


#endif // !_CONST_H_
