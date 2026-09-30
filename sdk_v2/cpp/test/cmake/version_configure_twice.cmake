# Copyright (c) Microsoft. All rights reserved.

file(REMOVE_RECURSE "${BINARY_DIR}")

function(configure_and_expect project_version expected_version)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${SOURCE_DIR}"
            -B "${BINARY_DIR}"
            -DTEST_PROJECT_VERSION=${project_version}
            -DFOUNDRY_LOCAL_SOURCE_DIR=${SOURCE_DIR}/../../..
            ${ARGN}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_output
        ERROR_VARIABLE configure_error
    )
    if(NOT configure_result EQUAL 0)
        message(FATAL_ERROR "Configure failed:\n${configure_output}\n${configure_error}")
    endif()

    file(READ "${BINARY_DIR}/resolved-version.txt" resolved_version)
    if(NOT resolved_version STREQUAL expected_version)
        message(FATAL_ERROR
            "Expected version '${expected_version}', got '${resolved_version}'")
    endif()
endfunction()

# Seed the old cache variable to reproduce an existing build directory, then
# reconfigure the same directory with a newer project version.
configure_and_expect("0.1.0" "0.1.0" -DFOUNDRY_LOCAL_VERSION_STRING=0.1.0)
configure_and_expect("2.0.1" "2.0.1")
configure_and_expect("2.0.1" "2.1.0-dev" -DFOUNDRY_LOCAL_PACKAGE_VERSION=2.1.0-dev)

file(READ "${BINARY_DIR}/CMakeCache.txt" cache_contents)
if(cache_contents MATCHES "FOUNDRY_LOCAL_VERSION_STRING")
    message(FATAL_ERROR "Legacy FOUNDRY_LOCAL_VERSION_STRING remained in CMakeCache.txt")
endif()