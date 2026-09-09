find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

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
