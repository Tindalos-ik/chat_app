find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(tcp_stream_test tcp_stream_test.cpp)
target_compile_features(tcp_stream_test PRIVATE cxx_std_17)
target_include_directories(tcp_stream_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ChatServer1/include)
target_link_libraries(tcp_stream_test PRIVATE GTest::gtest_main)
gtest_discover_tests(tcp_stream_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)

# 两组数据库用例链接同一份生产数据访问实现。运行前由用例检查隔离测试库配置；
# 普通 CTest/CI 环境不提供该配置时会明确跳过，绝不连接开发库。
find_package(redis++ CONFIG REQUIRED)
find_package(unofficial-mysql-connector-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_path(CHAT_TEST_BOOST_INCLUDE_DIR boost/property_tree/ptree.hpp REQUIRED)
find_path(CHAT_TEST_HIREDIS_INCLUDE_DIR hiredis/hiredis.h REQUIRED)
find_library(CHAT_TEST_HIREDIS_LIBRARY NAMES hiredis REQUIRED)

add_library(chat_mysql_test_support STATIC
    ${CMAKE_SOURCE_DIR}/ChatServer1/src/MysqlMgr.cpp
    ${CMAKE_SOURCE_DIR}/ChatServer1/src/ConfigMgr.cpp
    ${CMAKE_SOURCE_DIR}/ChatServer1/src/RedisMgr.cpp
    ${CMAKE_SOURCE_DIR}/ChatServer1/src/DistLock.cpp
    ${CMAKE_SOURCE_DIR}/ChatServer1/jsoncpp/jsoncpp.cpp
)
target_compile_features(chat_mysql_test_support PUBLIC cxx_std_17)
target_include_directories(chat_mysql_test_support PUBLIC
    ${CMAKE_SOURCE_DIR}/ChatServer1/include
    ${CHAT_TEST_BOOST_INCLUDE_DIR}
    ${CHAT_TEST_HIREDIS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/ChatServer1/jsoncpp/json
)
target_link_libraries(chat_mysql_test_support PUBLIC
    redis++::redis++
    unofficial::mysql-connector-cpp::connector
    ${CHAT_TEST_HIREDIS_LIBRARY}
    Threads::Threads
)

foreach(test_name IN ITEMS friend_transaction_test message_retry_offline_sync_test)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE
        GTest::gtest_main chat_mysql_test_support)
    gtest_discover_tests(${test_name} DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS integration)
endforeach()

find_package(Qt6 6.5 QUIET COMPONENTS Core Sql Widgets)
set(CHAT_APP_SQLITE_QT_MISMATCH FALSE)
if(Qt6_FOUND AND MSVC AND Qt6_DIR MATCHES "[/\\\\]mingw[^/\\\\]*[/\\\\]")
    message(STATUS "Qt6 uses MinGW while this build uses MSVC; using a separate SQLite test build with the desktop kit")
    set(CHAT_APP_SQLITE_QT_MISMATCH TRUE)
    set(Qt6_FOUND FALSE)
endif()
if(Qt6_FOUND)
    add_executable(local_chat_storage_test
        local_chat_storage_test.cpp
        "${CMAKE_SOURCE_DIR}/chat_app desktop/localchatstoragemgr.cpp")
    target_include_directories(local_chat_storage_test PRIVATE
        "${CMAKE_SOURCE_DIR}/chat_app desktop")
    target_link_libraries(local_chat_storage_test PRIVATE
        GTest::gtest_main Qt6::Core Qt6::Sql Qt6::Widgets)
    gtest_discover_tests(local_chat_storage_test DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS unit)
elseif(WIN32 AND CHAT_APP_SQLITE_QT_MISMATCH)
    # The desktop Qt kit may use MinGW while the root project uses MSVC. Never
    # link those ABIs together; build the SQLite test in a nested build using
    # the compiler and Qt kit recorded by the desktop's CMakeCache.txt.
    function(read_cache_value cache_file key output_variable)
        file(STRINGS "${cache_file}" _cache_entry REGEX "^${key}:[^=]*=")
        if(_cache_entry)
            list(GET _cache_entry 0 _cache_entry)
            string(REGEX REPLACE "^[^=]*=" "" _cache_value "${_cache_entry}")
            set(${output_variable} "${_cache_value}" PARENT_SCOPE)
        endif()
    endfunction()

    set(_desktop_build_root "${CMAKE_SOURCE_DIR}/chat_app desktop/build")
    file(GLOB_RECURSE _desktop_cache_files LIST_DIRECTORIES FALSE
        "${_desktop_build_root}/*/CMakeCache.txt")

    foreach(_desktop_cache IN LISTS _desktop_cache_files)
        read_cache_value("${_desktop_cache}" CMAKE_GENERATOR _desktop_generator)
        read_cache_value("${_desktop_cache}" CMAKE_CXX_COMPILER _desktop_compiler)
        read_cache_value("${_desktop_cache}" CMAKE_MAKE_PROGRAM _desktop_make_program)
        read_cache_value("${_desktop_cache}" Qt6_DIR _desktop_qt6_dir)
        if(_desktop_generator MATCHES "^Ninja"
           AND _desktop_compiler
           AND _desktop_make_program
           AND _desktop_qt6_dir
           AND EXISTS "${_desktop_qt6_dir}/Qt6Config.cmake")
            set(_desktop_cache_selected "${_desktop_cache}")
            break()
        endif()
    endforeach()

    set(_vcpkg_root "$ENV{VCPKG_ROOT}")
    if(NOT _vcpkg_root AND DEFINED VCPKG_INSTALLED_DIR)
        get_filename_component(_vcpkg_root "${VCPKG_INSTALLED_DIR}" DIRECTORY)
    endif()
    if(NOT _vcpkg_root AND CMAKE_TOOLCHAIN_FILE MATCHES "[/\\\\]scripts[/\\\\]buildsystems[/\\\\]vcpkg.cmake$")
        get_filename_component(_vcpkg_root "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
        get_filename_component(_vcpkg_root "${_vcpkg_root}" DIRECTORY)
        get_filename_component(_vcpkg_root "${_vcpkg_root}" DIRECTORY)
    endif()

    set(_gtest_source_dir "")
    if(_vcpkg_root)
        file(GLOB _gtest_source_candidates LIST_DIRECTORIES TRUE
            "${_vcpkg_root}/buildtrees/gtest/src/*")
        foreach(_candidate IN LISTS _gtest_source_candidates)
            if(EXISTS "${_candidate}/CMakeLists.txt")
                set(_gtest_source_dir "${_candidate}")
                break()
            endif()
        endforeach()
    endif()

    if(_desktop_cache_selected AND _gtest_source_dir)
        get_filename_component(_desktop_qt_root
            "${_desktop_qt6_dir}/../../.." ABSOLUTE)
        set(_sqlite_build_dir "${CMAKE_BINARY_DIR}/sqlite-client-kit")

        add_custom_target(local_chat_storage_test ALL
            COMMAND "${CMAKE_COMMAND}"
                -S "${CMAKE_CURRENT_SOURCE_DIR}/sqlite"
                -B "${_sqlite_build_dir}"
                -G "${_desktop_generator}"
                "-DCMAKE_CXX_COMPILER=${_desktop_compiler}"
                "-DCMAKE_MAKE_PROGRAM=${_desktop_make_program}"
                "-DCMAKE_BUILD_TYPE=$<CONFIG>"
                "-DQt6_DIR=${_desktop_qt6_dir}"
                "-DGTEST_SOURCE_DIR=${_gtest_source_dir}"
            COMMAND "${CMAKE_COMMAND}" --build "${_sqlite_build_dir}"
                --config "$<CONFIG>" --target local_chat_storage_test
            COMMENT "Building SQLite test with the desktop Qt kit and compiler"
            USES_TERMINAL
            VERBATIM
        )

        add_test(NAME LocalChatStorageTest
            COMMAND "${CMAKE_COMMAND}"
                "-DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}"
                "-DSQLITE_BUILD_DIR=${_sqlite_build_dir}"
                "-DTEST_CONFIGURATION=$<CONFIG>"
                "-DQT_BIN=${_desktop_qt_root}/bin"
                "-DQT_PLUGIN_DIR=${_desktop_qt_root}/plugins"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/sqlite/run_nested_ctest.cmake"
        )
        set_tests_properties(LocalChatStorageTest PROPERTIES LABELS unit)
    elseif(CHAT_APP_TESTS_ONLY)
        message(FATAL_ERROR
            "SQLite test needs the desktop Ninja/Qt CMakeCache and the vcpkg GoogleTest source tree")
    else()
        message(STATUS
            "Qt compiler mismatch; SQLite test is skipped because the desktop Qt build cache or vcpkg GoogleTest sources were not found")
    endif()
elseif(CHAT_APP_TESTS_ONLY)
    message(FATAL_ERROR "Qt6 Core/Sql/Widgets is required for the client SQLite tests")
else()
    message(STATUS "Qt6 was not found; skipping local_chat_storage_test")
endif()

add_executable(connection_capacity_test
    connection_capacity_test.cpp
)

target_compile_features(connection_capacity_test PRIVATE cxx_std_17)
target_link_libraries(connection_capacity_test PRIVATE GTest::gtest_main)

if(WIN32)
    target_link_libraries(connection_capacity_test PRIVATE ws2_32)
endif()

gtest_discover_tests(connection_capacity_test
    PROPERTIES LABELS capacity
)
