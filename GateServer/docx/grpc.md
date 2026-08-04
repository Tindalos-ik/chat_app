# grpc

使用grpc实现分布式服务器架构，充分利用好不同语言的特性，比如使用nodejs写一个验证服务器，然后在c++这边写一个客户端去调用这个服务器的接口

```
┌─────────────────────────────────────────────────────────────────────┐
│                         `.proto` 文件                               │
│              （双方共同遵守的"合同/规则"）                            │
│                                                                     │
│   service VarifyService {                                           │
│       rpc GetVarifyCode (GetVarifyReq) returns (GetVarifyRsp);      │
│   }                                                                 │
└─────────────────────────────────────────────────────────────────────┘
                    │                              │
                    │ 编译生成                       │ 编译生成
                    ▼                              ▼
┌───────────────────────────────┐  ┌───────────────────────────────┐
│     C++ 客户端代码              │  │     Node.js 服务端代码         │
│     (GateServer)              │  │     (VarifyServer)            │
│                               │  │                               │
│  VarifyGrpcClient::           │  │  server.addService(           │
│    GetVarifyCode(email) {     │  │    VarifyService.service,     │
│      stub_->GetVarifyCode(    │  │    { GetVarifyCode: ... }      │
│        request, &reply);      │  │  );                           │
│  }                            │  │                               │
└───────────────────────────────┘  └───────────────────────────────┘
                    │                              │
                    │  1. 调用 GetVarifyCode()      │
                    │ ─────────────────────────────►│
                    │                               │
                    │  2. 发送验证码到用户邮箱       │
                    │                               │
                    │  3. 返回 GetVarifyRsp         │
                    │ ◄─────────────────────────────│
                    │                               │
```

