# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

if(NOT RUN_CONSUMER)
    if(CMAKE_CROSSCOMPILING OR ANDROID OR IOS)
        return()
    endif()
    foreach(config Debug Release)
        foreach(first ON OFF)
            add_test(NAME NanoarrowConsumer-${config}-${first}
                COMMAND "${CMAKE_COMMAND}"
                    "-DRUN_CONSUMER=ON"
                    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/nanoarrow-consumer"
                    "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}/nanoarrow-consumer-${config}-${first}"
                    "-DCONFIG=${config}" "-DFIRST=${first}"
                    "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                    "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                    "-DTOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
                    "-DINSTALLED=${VCPKG_INSTALLED_DIR}" "-DTRIPLET=${VCPKG_TARGET_TRIPLET}"
                    "-DCTEST=${CMAKE_CTEST_COMMAND}"
                    -P "${CMAKE_CURRENT_LIST_FILE}")
            set_tests_properties(NanoarrowConsumer-${config}-${first} PROPERTIES TIMEOUT 180)
        endforeach()
    endforeach()
    return()
endif()

function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "nanoarrow consumer failed (${result}): ${ARGV}")
    endif()
endfunction()

set(generator_args)
if(PLATFORM)
    list(APPEND generator_args -A "${PLATFORM}")
endif()
if(TOOLSET)
    list(APPEND generator_args -T "${TOOLSET}")
endif()
run("${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}" -G "${GENERATOR}" ${generator_args}
    "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}" "-DVCPKG_INSTALLED_DIR=${INSTALLED}"
    "-DVCPKG_TARGET_TRIPLET=${TRIPLET}" "-DVCPKG_MANIFEST_MODE=OFF"
    "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DTEST_CONFIG=${CONFIG}" "-DCALLER_FLATCC_FIRST=${FIRST}")
run("${CMAKE_COMMAND}" --build "${BINARY}" --config "${CONFIG}")
if(CONFIG STREQUAL "Debug")
    set(runtime_dir "${INSTALLED}/${TRIPLET}/debug/bin")
else()
    set(runtime_dir "${INSTALLED}/${TRIPLET}/bin")
endif()
if(CMAKE_HOST_WIN32)
    set(ENV{PATH} "${runtime_dir};$ENV{PATH}")
endif()
run("${CTEST}" --test-dir "${BINARY}" -C "${CONFIG}" --no-tests=error --output-on-failure)
