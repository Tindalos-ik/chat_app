cmake_minimum_required(VERSION 3.10)
project(distribute_lock LANGUAGES CXX)

# 该组件可单独构建；hiredis 由项目已安装的 redis-plus-plus vcpkg 包提供。
find_package(redis++ CONFIG REQUIRED)

add_library(distribute_lock STATIC
    src/RedisDistributedLock.cpp
)

target_compile_features(distribute_lock PUBLIC cxx_std_17)
target_include_directories(distribute_lock
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(distribute_lock PUBLIC redis++::redis++)
