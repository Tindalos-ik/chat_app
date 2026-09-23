cmake_minimum_required(VERSION 3.21)
project(chat_app_sqlite_test LANGUAGES CXX)

include(CTest)
include(GoogleTest)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Sql Widgets)

if(NOT GTEST_SOURCE_DIR OR NOT EXISTS "${GTEST_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "GTEST_SOURCE_DIR must point to a GoogleTest source checkout")
endif()

# The root vcpkg build uses MSVC while the desktop Qt kit may use MinGW.
# Build GoogleTest from source here so every object uses the desktop kit compiler.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)

get_filename_component(CHAT_APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(local_chat_storage_test
    "${CMAKE_CURRENT_LIST_DIR}/../local_chat_storage_test.cpp"
    "${CHAT_APP_ROOT}/chat_app desktop/localchatstoragemgr.cpp"
)
target_compile_features(local_chat_storage_test PRIVATE cxx_std_17)
target_include_directories(local_chat_storage_test PRIVATE
    "${CHAT_APP_ROOT}/chat_app desktop"
)
target_link_libraries(local_chat_storage_test PRIVATE
    GTest::gtest_main
    Qt6::Core
    Qt6::Sql
    Qt6::Widgets
)

gtest_discover_tests(local_chat_storage_test
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit
)
