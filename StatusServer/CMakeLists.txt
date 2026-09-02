cmake_minimum_required(VERSION 3.10)
project(StatusServer)

message(STATUS "CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")

add_compile_options(/utf-8 /wd4819) #忽略编码警告

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(redis++ CONFIG REQUIRED)

# 指定 vcpkg 工具链路径
set(CMAKE_TOOLCHAIN_FILE "D:/cppsoft/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE PATH "")

# 依赖由顶层 CMake 查找，StatusServer 只声明自身额外依赖。
find_package(redis++ CONFIG REQUIRED)


# ==================== 项目源文件 ====================
set(JSONCPP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/jsoncpp.cpp
)

set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConfigMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsioIOServicePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MysqlMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RedisMgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StatusServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StatusServiceImpl.cpp
)

link_directories("D:/cppsoft/vcpkg/installed/x64-windows/lib") # 添加 vcpkg 的 lib 路径，库文件搜索路径

# ==================== 可执行文件 ====================
add_executable(${PROJECT_NAME} 
    ${JSONCPP_SOURCES}  
    ${SOURCE_FILES}
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
    chat_proto
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


