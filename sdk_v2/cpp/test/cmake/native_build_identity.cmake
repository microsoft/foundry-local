# Copyright (c) Microsoft. All rights reserved.

set(fixture_source "${BINARY_DIR}/source")
set(fixture_build "${BINARY_DIR}/build")
file(MAKE_DIRECTORY "${fixture_source}/src" "${fixture_source}/include")
configure_file("${SOURCE_DIR}/CMakeLists.txt" "${fixture_source}/CMakeLists.txt" COPYONLY)
file(WRITE "${fixture_source}/src/native.cc" "int native_value = 1;\n")
file(WRITE "${fixture_source}/include/native.h" "#define NATIVE_VALUE 1\n")

function(run)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Command failed (${ARGV}):\n${output}\n${error}")
    endif()
endfunction()

run("${CMAKE_COMMAND}" -S "${fixture_source}" -B "${fixture_build}" -G "${GENERATOR}"
    "-DFOUNDRY_LOCAL_SOURCE_DIR=${SOURCE_DIR}/../../..")
file(READ "${fixture_build}/native-build-id.txt" initial_id)
run("${CMAKE_COMMAND}" -S "${fixture_source}" -B "${fixture_build}")
file(READ "${fixture_build}/native-build-id.txt" unchanged_id)
if(NOT unchanged_id STREQUAL initial_id)
    message(FATAL_ERROR "Unchanged configure replaced the native build identity")
endif()

# Change tracked inputs, then build without an explicit configure to exercise automatic regeneration.
foreach(input IN ITEMS src/native.cc include/native.h)
    run("${CMAKE_COMMAND}" -E sleep 1)
    file(APPEND "${fixture_source}/${input}" "// changed native input\n")
    run("${CMAKE_COMMAND}" --build "${fixture_build}")
    file(READ "${fixture_build}/native-build-id.txt" rebuilt_id)
    if(rebuilt_id STREQUAL unchanged_id)
        message(FATAL_ERROR "Incremental rebuild retained the native build identity after changing ${input}")
    endif()
    set(unchanged_id "${rebuilt_id}")
endforeach()

run("${CMAKE_COMMAND}" --build "${fixture_build}")
file(READ "${fixture_build}/native-build-id.txt" stable_id)
if(NOT stable_id STREQUAL rebuilt_id)
    message(FATAL_ERROR "Unchanged incremental build replaced the native build identity")
endif()
