cmake_minimum_required(VERSION 3.10)
project(ChatServer1)

add_compile_options(/utf-8 /wd4819) #忽略编码警告

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#这些包都是vcpkg管理的
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)


# ==================== 获取工具路径 ====================
get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
if(NOT Protobuf_PROTOC_EXECUTABLE)
    get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
endif()

get_target_property(gRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION_RELEASE)
if(NOT gRPC_CPP_PLUGIN)
    get_target_property(gRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)
endif()

# ==================== Proto 文件自动编译 ====================
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)

# 生成到项目根目录下的 proto_gen 文件夹
set(PROTO_GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto_gen)

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

set(GENERATED_FILES "")
foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    # 生成 protobuf 代码（处理 message） ->> .pb.cc .pb.h
    execute_process(
        COMMAND ${Protobuf_PROTOC_EXECUTABLE} 
            --cpp_out=${PROTO_GEN_DIR}
            --proto_path=${PROTO_DIR}
            ${PROTO_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    
    # 生成 grpc 代码（处理 service） ->> .grpc.pb.cc .grpc.pb.h
    execute_process(
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN}
            --proto_path=${PROTO_DIR}
            ${PROTO_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    
    list(APPEND GENERATED_FILES 
        ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc
        ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h
        ${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.cc
        ${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.h
    )
endforeach()

# ==================== 项目源文件 ====================
set(JSONCPP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/jsoncpp.cpp
)

set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LogicSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConfigMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsioIOServicePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MysqlMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RedisMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StatusGrpcClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSession.cpp
)

link_directories("D:/cppsoft/vcpkg/installed/x64-windows/lib") # 添加 vcpkg 的 lib 路径，库文件搜索路径

# ==================== 可执行文件 ====================
add_executable(${PROJECT_NAME} 
    ChatServer.cpp
    ${JSONCPP_SOURCES}  
    ${SOURCE_FILES}
    ${GENERATED_FILES}
    include/MsgNode.h
)

# ==================== 头文件搜索路径 ====================
target_include_directories(${PROJECT_NAME} PRIVATE 
    "C:/local/boost_1_91_0"
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/json
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTO_GEN_DIR}  # 生成的头文件路径
    "D:/cppsoft/vcpkg/installed/x64-windows/include"   # 添加 vcpkg 的 include 路径
)

# ==================== 链接库 ====================
target_link_libraries(${PROJECT_NAME} 
    gRPC::grpc++                        #链接grpc-c++库
    protobuf::libprotobuf               #链接protobuf库
    redis++::redis++                    #链接redis-c++库
    mysqlcppconnx                        #链接mysql-c++库
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


