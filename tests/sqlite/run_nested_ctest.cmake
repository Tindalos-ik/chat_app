if(NOT CTEST_EXECUTABLE OR NOT SQLITE_BUILD_DIR OR NOT TEST_CONFIGURATION)
    message(FATAL_ERROR "Nested SQLite CTest invocation is missing required paths")
endif()

set(_runtime_path "$ENV{PATH}")
if(QT_BIN)
    set(_runtime_path "${QT_BIN};${_runtime_path}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${_runtime_path}"
        "QT_PLUGIN_PATH=${QT_PLUGIN_DIR}"
        "${CTEST_EXECUTABLE}"
        --test-dir "${SQLITE_BUILD_DIR}"
        -C "${TEST_CONFIGURATION}"
        --output-on-failure
    RESULT_VARIABLE _ctest_result
)

if(NOT _ctest_result EQUAL 0)
    message(FATAL_ERROR "Nested SQLite CTest failed: ${_ctest_result}")
endif()
