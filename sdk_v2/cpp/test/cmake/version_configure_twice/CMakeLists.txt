# Copyright (c) Microsoft. All rights reserved.
cmake_minimum_required(VERSION 3.20)

project(version_configure_twice VERSION "${TEST_PROJECT_VERSION}" LANGUAGES NONE)

include("${FOUNDRY_LOCAL_SOURCE_DIR}/cmake/FoundryLocalVersion.cmake")
file(WRITE "${CMAKE_BINARY_DIR}/resolved-version.txt" "${FOUNDRY_LOCAL_VERSION_STRING}")