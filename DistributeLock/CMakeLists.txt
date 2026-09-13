cmake_minimum_required(VERSION 3.10)

# 单独配置 DistributeLock 时，不会经过根目录 CMakePresets.json；因此在第一次
# project() 之前指定与主工程相同的 vcpkg 工具链。调用方若通过 -DCMAKE_TOOLCHAIN_FILE
# 传入了其他位置，本默认值不会覆盖它。
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "D:/cppsoft/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg target triplet")
endif()

project(distribute_lock LANGUAGES CXX)

# 该组件可单独构建；hiredis 由项目已安装的 redis-plus-plus vcpkg 包提供。
find_package(redis++ CONFIG REQUIRED)

# 源码和测试中包含中文注释、测试输出。独立构建时不会继承根工程的 /utf-8，
# 因此在 Windows/MSVC 下显式指定 UTF-8，避免按本机代码页解析为语法错误。
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(distribute_lock STATIC
    src/RedisDistributedLock.cpp
)

target_compile_features(distribute_lock PUBLIC cxx_std_17)
target_include_directories(distribute_lock
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(distribute_lock PUBLIC redis++::redis++)

# 这是可直接运行的集成测试，不使用 GTest，便于学习时单独执行。
add_executable(distribute_lock_test test.cpp)
target_compile_features(distribute_lock_test PRIVATE cxx_std_17)
target_link_libraries(distribute_lock_test PRIVATE distribute_lock)

# CMake Tools 的“运行目标”会直接执行 distribute_lock_test.exe。将 hiredis DLL 复制到
# exe 同目录后，VS Code 无需额外修改 PATH 也能找到运行时依赖。
if(WIN32)
    add_custom_command(TARGET distribute_lock_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:hiredis::hiredis>
            $<TARGET_FILE_DIR:distribute_lock_test>
        VERBATIM
    )
endif()

# Visual Studio/CMake Tools 将测试程序作为默认可运行 target。
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT distribute_lock_test)
