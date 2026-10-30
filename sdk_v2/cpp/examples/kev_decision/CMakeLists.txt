# Copyright (c) Microsoft. All rights reserved.

add_executable(kev_decision_example main.cc)
target_include_directories(
    kev_decision_example
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
)
target_link_libraries(kev_decision_example PRIVATE foundry_local_static)
