# Copyright (c) Microsoft. All rights reserved.
cmake_minimum_required(VERSION 3.20)

if(DEFINED EXPECT_ENABLED)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/chat-template-kwargs.cmake")
    if(NOT "${FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS}" STREQUAL "${EXPECT_ENABLED}")
        message(FATAL_ERROR "Unexpected kwargs capability: ${FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS}")
    endif()
    if(DEFINED NEXT_VERSION)
        set(ORT_GENAI_VERSION "${NEXT_VERSION}")
        include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/chat-template-kwargs.cmake")
        if(NOT "${FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS}" STREQUAL "${EXPECT_NEXT}")
            message(FATAL_ERROR "Kwargs capability was not recomputed after changing versions")
        endif()
    endif()
    return()
endif()

function(check_case name expected error_pattern)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DEXPECT_ENABLED=${expected}" ${ARGN} -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(error_pattern)
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${error_pattern}")
            message(FATAL_ERROR "${name}: expected configuration error '${error_pattern}':\n${output}${error}")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}:\n${output}${error}")
    endif()
    message(STATUS "${name}: passed")
endfunction()

check_case(stable ON "" -DORT_GENAI_VERSION=0.16.0 -DFOUNDRY_LOCAL_REQUIRE_CHAT_TEMPLATE_KWARGS=ON)
check_case(newer-stable ON "" -DORT_GENAI_VERSION=0.17.0)
check_case(older-stable OFF "" -DORT_GENAI_VERSION=0.15.2)
check_case(unknown-version OFF "")
check_case(older-nightly OFF "" -DORT_GENAI_VERSION=0.16.0-dev1001407372)
check_case(first-supported-nightly ON "" -DORT_GENAI_VERSION=0.16.0-dev1001407373)
check_case(newer-nightly ON "" -DORT_GENAI_VERSION=0.16.0-dev1001407374)
check_case(changed-version ON "" -DORT_GENAI_VERSION=0.16.0 -DNEXT_VERSION=0.15.2 -DEXPECT_NEXT=OFF)
check_case(local-auto OFF "" "-DORT_GENAI_HOME=local-genai")
check_case(local-ignores-stale-package-version OFF "" "-DORT_GENAI_HOME=local-genai" -DORT_GENAI_VERSION=0.16.0)
check_case(local-explicit-on ON "" "-DORT_GENAI_HOME=local-genai"
    -DFOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=ON -DFOUNDRY_LOCAL_REQUIRE_CHAT_TEMPLATE_KWARGS=ON)
check_case(local-explicit-off OFF "" "-DORT_GENAI_HOME=local-genai" -DFOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=OFF)
check_case(package-explicit-off OFF "" -DORT_GENAI_VERSION=0.16.0 -DFOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=OFF)
check_case(lowercase-on ON "" "-DORT_GENAI_HOME=local-genai" -DFOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=on)
check_case(local-required OFF "support is required" "-DORT_GENAI_HOME=local-genai"
    -DFOUNDRY_LOCAL_REQUIRE_CHAT_TEMPLATE_KWARGS=ON)
check_case(older-required OFF "support is required" -DORT_GENAI_VERSION=0.15.2
    -DFOUNDRY_LOCAL_REQUIRE_CHAT_TEMPLATE_KWARGS=ON)
check_case(disabled-required OFF "support is required" -DORT_GENAI_VERSION=0.16.0
    -DFOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=OFF -DFOUNDRY_LOCAL_REQUIRE_CHAT_TEMPLATE_KWARGS=ON)
check_case(invalid-mode OFF "must be AUTO, ON or OFF" -DFOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=invalid)
