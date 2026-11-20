# 配置第一个聊天服务实例的编译及运行配置复制。
cmake_minimum_required(VERSION 3.10)
project(ChatServer1)

add_compile_options(/utf-8 /wd4819) #忽略编码警告

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(redis++ CONFIG REQUIRED)


# ==================== 项目源文件 ====================
set(JSONCPP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/jsoncpp.cpp
)

set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LogicSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VoiceCall.cpp # 注册好友双向语音业务与专用跨服RPC。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConfigMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsioIOServicePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MysqlMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GroupManagement.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RedisMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StatusGrpcClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ChatGrpcClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UserMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ChatServiceImpl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DistLock.cpp
)

# ==================== 可执行文件 ====================
add_executable(${PROJECT_NAME} 
    ChatServer.cpp
    ${JSONCPP_SOURCES}  
    ${SOURCE_FILES}
)

# ==================== 头文件搜索路径 ====================
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CHAT_APP_BOOST_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/json
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ==================== 链接库 ====================
target_link_libraries(${PROJECT_NAME} 
    chat_proto
    gRPC::grpc++                        #链接grpc-c++库
    protobuf::libprotobuf               #链接protobuf库
    redis++::redis++                    #链接redis-c++库
    unofficial::mysql-connector-cpp::connector
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} 
        ws2_32
        mswsock
    )
endif()

# 配置文件，也就是将config.ini文件拷贝到可执行文件所在目录
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.ini
    ${CMAKE_CURRENT_BINARY_DIR}/Debug/config.ini
    COPYONLY
)


