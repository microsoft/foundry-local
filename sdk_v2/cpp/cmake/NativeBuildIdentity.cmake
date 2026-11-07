# Copyright (c) Microsoft. All rights reserved.

function(foundry_local_refresh_native_build_id source_dir)
    file(GLOB_RECURSE native_inputs CONFIGURE_DEPENDS RELATIVE "${source_dir}"
        "${source_dir}/src/*.cc" "${source_dir}/src/*.h" "${source_dir}/src/*.in"
        "${source_dir}/include/*.h" "${source_dir}/tools/*.cc" "${source_dir}/cmake/*.cmake")
    list(APPEND native_inputs CMakeLists.txt)
    list(SORT native_inputs)

    string(CONCAT inputs
        "${PROJECT_VERSION};${FOUNDRY_LOCAL_PACKAGE_VERSION};${FOUNDRY_LOCAL_BUILD_SERVICE};"
        "${FOUNDRY_LOCAL_COMPILE_OPTIONS};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};"
        "${CMAKE_BUILD_TYPE};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_CXX_FLAGS_RELEASE};"
        "${CMAKE_CXX_FLAGS_RELWITHDEBINFO};${CMAKE_CXX_FLAGS_MINSIZEREL}")
    foreach(input IN LISTS native_inputs)
        file(SHA256 "${source_dir}/${input}" hash)
        string(APPEND inputs ";${input}:${hash}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_dir}/${input}")
    endforeach()
    string(SHA256 fingerprint "${inputs}")

    if(NOT DEFINED FOUNDRY_LOCAL_NATIVE_BUILD_ID OR
       NOT "${FOUNDRY_LOCAL_NATIVE_BUILD_INPUTS}" STREQUAL "${fingerprint}")
        string(RANDOM LENGTH 32 ALPHABET 0123456789ABCDEF native_build_id)
        set(FOUNDRY_LOCAL_NATIVE_BUILD_ID "${native_build_id}" CACHE INTERNAL "Native runtime build identity" FORCE)
        set(FOUNDRY_LOCAL_NATIVE_BUILD_INPUTS "${fingerprint}" CACHE INTERNAL "Native build input fingerprint" FORCE)
    endif()
endfunction()
