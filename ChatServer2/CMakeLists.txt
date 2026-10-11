cmake_minimum_required(VERSION 3.10)
project(ChatServer2)

add_compile_options(/utf-8 /wd4819) #蹇界暐缂栫爜璀﹀憡

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(redis++ CONFIG REQUIRED)


# ==================== 椤圭洰婧愭枃浠?====================
set(JSONCPP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/jsoncpp.cpp
)

set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LogicSystem.cpp
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

link_directories("D:/cppsoft/vcpkg/installed/x64-windows/lib") # 娣诲姞 vcpkg 鐨?lib 璺緞锛屽簱鏂囦欢鎼滅储璺緞

# ==================== 鍙墽琛屾枃浠?====================
add_executable(${PROJECT_NAME} 
    ChatServer.cpp
    ${JSONCPP_SOURCES}  
    ${SOURCE_FILES}
)

# ==================== 澶存枃浠舵悳绱㈣矾寰?====================
target_include_directories(${PROJECT_NAME} PRIVATE 
    "C:/local/boost_1_91_0"
    ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/json
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "D:/cppsoft/vcpkg/installed/x64-windows/include"   # 娣诲姞 vcpkg 鐨?include 璺緞
)

# ==================== 閾炬帴搴?====================
target_link_libraries(${PROJECT_NAME} 
    chat_proto
    gRPC::grpc++                        #閾炬帴grpc-c++搴?
    protobuf::libprotobuf               #閾炬帴protobuf搴?
    redis++::redis++                    #閾炬帴redis-c++搴?
    mysqlcppconnx                        #閾炬帴mysql-c++搴?
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} 
        ws2_32
        mswsock
    )
endif()

# 閰嶇疆鏂囦欢锛屼篃灏辨槸灏哻onfig.ini鏂囦欢鎷疯礉鍒板彲鎵ц鏂囦欢鎵€鍦ㄧ洰褰?
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.ini
    ${CMAKE_CURRENT_BINARY_DIR}/Debug/config.ini
    COPYONLY
)


