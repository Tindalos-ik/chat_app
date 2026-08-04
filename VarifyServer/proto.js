// 引入模块
const path = require('path');
const grpc = require('@grpc/grpc-js');  // 使用 @grpc/grpc-js，不是 'grpc'
const protoLoader = require('@grpc/proto-loader');

// .proto 文件路径
const PROTO_PATH = path.join(__dirname, 'message.proto');

// 解析 .proto 文件
const packageDefinition = protoLoader.loadSync(PROTO_PATH, {
    keepCase: true,
    longs: String,
    enums: String,
    defaults: true,
    oneofs: true
});

// 获取 proto 文件中的定义
const protoDescriptor = grpc.loadPackageDefinition(packageDefinition);

// 导出服务
// message.proto 中有：package message; service VarifyService
const message_proto = protoDescriptor.message;  // message 是 package 名

module.exports = message_proto;  //把当前文件中的变量、函数、对象导出，让其他文件可以引用