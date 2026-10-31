# Copyright (c) Microsoft. All rights reserved.
cmake_minimum_required(VERSION 3.20)
project(NativeBuildIdentity NONE)

include("${FOUNDRY_LOCAL_SOURCE_DIR}/cmake/NativeBuildIdentity.cmake")
foundry_local_refresh_native_build_id("${CMAKE_CURRENT_SOURCE_DIR}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/native-build-id.txt" "${FOUNDRY_LOCAL_NATIVE_BUILD_ID}")
add_custom_target(native-identity ALL COMMAND "${CMAKE_COMMAND}" -E true)
