# Copyright (c) Microsoft. All rights reserved.
# Shared compiler flags for the foundry_local project.
# Flags are stored in variables rather than applied globally via add_compile_options
# so that third-party FetchContent dependencies (e.g., oat++) are not affected by /WX.

if(MSVC)
    # /Zc:__cplusplus makes MSVC report the actual C++ standard version in __cplusplus
    # (without it, __cplusplus is always 199711L). Required so that ort_genai.h enables
    # its C++20 std::span-based APIs (guarded by __cplusplus >= 202002L).
    set(FOUNDRY_LOCAL_COMPILE_OPTIONS /W4 /WX /utf-8 /Zc:__cplusplus /guard:cf)
else()
    set(FOUNDRY_LOCAL_COMPILE_OPTIONS -Wall -Wextra -Wpedantic -Werror)
endif()

# C++20 optional features behind a define
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++20" HAS_CXX20)
if(HAS_CXX20)
    add_compile_definitions(FOUNDRY_LOCAL_HAS_CXX20=1)
endif()
