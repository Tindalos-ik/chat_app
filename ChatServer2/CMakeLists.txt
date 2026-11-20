# 配置第二个聊天服务实例的编译及运行配置复制。
cmake_minimum_required(VERSION 3.10)
project(ChatServer2)

add_compile_options(/utf-8 /wd4819) #韫囩晫鏆愮紓鏍垳鐠€锕€鎲?

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(redis++ CONFIG REQUIRED)


# ==================== 妞ゅ湱娲板┃鎰瀮娴?====================
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

# ==================== 閸欘垱澧界悰灞炬瀮娴?====================
add_executable(${PROJECT_NAME} 
    ChatServer.cpp
    ${JSONCPP_SOURCES}  
    ${SOURCE_FILES}
)

# ==================== 婢跺瓨鏋冩禒鑸垫偝缁便垼鐭惧?====================
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CHAT_APP_BOOST_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/json
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ==================== 闁剧偓甯存惔?====================
target_link_libraries(${PROJECT_NAME} 
    chat_proto
    gRPC::grpc++                        #闁剧偓甯磄rpc-c++鎼?
    protobuf::libprotobuf               #闁剧偓甯磒rotobuf鎼?
    redis++::redis++                    #闁剧偓甯磖edis-c++鎼?
    unofficial::mysql-connector-cpp::connector
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} 
        ws2_32
        mswsock
    )
endif()

# 闁板秶鐤嗛弬鍥︽閿涘奔绡冪亸杈ㄦЦ鐏忓摶onfig.ini閺傚洣娆㈤幏鐤閸掓澘褰查幍褑顢戦弬鍥︽閹碘偓閸︺劎娲拌ぐ?
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.ini
    ${CMAKE_CURRENT_BINARY_DIR}/Debug/config.ini
    COPYONLY
)


