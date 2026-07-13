# Copyright (c) Microsoft. All rights reserved.
cmake_minimum_required(VERSION 3.20)

# Android: web service not applicable on device.
# Detect Android early via the vcpkg triplet name because the ANDROID variable
# isn't defined until the NDK toolchain runs during project().
if(VCPKG_TARGET_TRIPLET MATCHES "android")
    set(FOUNDRY_LOCAL_BUILD_SERVICE OFF CACHE BOOL "" FORCE)
endif()

# Activate vcpkg "service" feature (installs oatpp) unless service is disabled.
# Must run before project() because that's when vcpkg resolves packages.
if(NOT DEFINED FOUNDRY_LOCAL_BUILD_SERVICE OR FOUNDRY_LOCAL_BUILD_SERVICE)
    list(APPEND VCPKG_MANIFEST_FEATURES "service")
endif()

project(foundry_local VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Organize projects into solution folders in Visual Studio
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Place DLLs and EXEs in a common bin directory so they find each other at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# On macOS, ensure all binaries look in the bin directory first when resolving
# @rpath dylibs.  The versioned ORT symlink (libonnxruntime.<ver>.dylib) lives
# here, and we want it found before any system-installed copy.
if(APPLE)
    list(APPEND CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}/bin")
endif()

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------
option(FOUNDRY_LOCAL_BUILD_TESTS "Build unit tests" ON)
option(FOUNDRY_LOCAL_BUILD_EXAMPLES "Build example programs" ON)
option(FOUNDRY_LOCAL_BUILD_SERVICE "Build web service support (requires oat++)" ON)
option(FOUNDRY_LOCAL_ENABLE_ASAN "Enable AddressSanitizer + UndefinedBehaviorSanitizer (Linux only)" OFF)

# Android: interactive examples and host tools don't run on device
if(ANDROID)
    set(FOUNDRY_LOCAL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
endif()

# --------------------------------------------------------------------------
# Compiler options
# --------------------------------------------------------------------------
include(cmake/CompilerOptions.cmake)

# Opt-in AddressSanitizer / UBSan instrumentation (Linux only).
include(cmake/Sanitizers.cmake)

# --------------------------------------------------------------------------
# Dependencies via vcpkg (manifest mode)
# --------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(azure-storage-blobs-cpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)

if(FOUNDRY_LOCAL_BUILD_SERVICE)
    find_package(oatpp CONFIG REQUIRED)
endif()

if(FOUNDRY_LOCAL_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
endif()

# ORT and ORT GenAI — acquired via FetchContent from nuget.org.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(OnnxRuntimeGenAI REQUIRED)
find_package(OnnxRuntime REQUIRED)

# WinML EP Catalog — Windows-only, for hardware EP discovery and download. The
# WinML 2.x runtime is reg-free, delay-loaded, and degrades gracefully when the
# DLL is absent, so it is always included on Windows; other platforms skip it.
if(WIN32)
    find_package(WinMLEpCatalog)
endif()

# --------------------------------------------------------------------------
# Library target
# --------------------------------------------------------------------------

# Collect header files so they appear in Visual Studio Solution Explorer.
# These are NOT used for build dependency tracking — CMake handles that
# via include paths. They are listed as sources purely for IDE visibility.
file(GLOB_RECURSE FOUNDRY_LOCAL_PUBLIC_HEADERS "include/*.h")
file(GLOB_RECURSE FOUNDRY_LOCAL_INTERNAL_HEADERS "src/*.h")

set(FOUNDRY_LOCAL_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND FOUNDRY_LOCAL_PLATFORM_SOURCES
        src/util/stacktrace_windows.cc
        src/platform/windows/path.cc
        src/platform/windows/file_io.cc
        src/platform/windows/cross_process_file_lock.cc
    )
else()
    list(APPEND FOUNDRY_LOCAL_PLATFORM_SOURCES
        src/util/stacktrace_posix.cc
        src/platform/posix/path.cc
        src/platform/posix/file_io.cc
        src/platform/posix/cross_process_file_lock.cc
    )
endif()

# WinML EP bootstrapper is only built when the WinML EP catalog package is
# available — the source unconditionally references its headers/types, which
# exist only on Windows. manager.cc gates the call site on
# FOUNDRY_LOCAL_HAS_EP_CATALOG so DiscoverProviders is only invoked when this
# translation unit is linked in.
if(WinMLEpCatalog_FOUND)
    list(APPEND FOUNDRY_LOCAL_PLATFORM_SOURCES
        src/ep_detection/winml_ep_bootstrapper.cc
    )
endif()

if(ANDROID)
    list(APPEND FOUNDRY_LOCAL_PLATFORM_SOURCES src/platform/android/ssl_cert_checker.cc)
endif()

set(FOUNDRY_LOCAL_SOURCES
    src/c_api.cc
    src/items/item.cc
    src/items/image_item.cc
    src/items/message_item.cc
    src/items/speech_segment_item.cc
    src/catalog/base_model_catalog.cc
    src/catalog/azure_model_catalog.cc
    src/catalog/azure_catalog_client.cc
    src/catalog/azure_catalog_models.cc
    src/catalog/catalog_cache.cc
    src/catalog/catalog_client.cc
    src/catalog/local_model_scanner.cc
    src/inferencing/generative/audio/audio_generator.cc
    src/inferencing/generative/audio/audio_session.cc
    src/inferencing/generative/audio/onnx_audio_generator.cc
    src/inferencing/generative/audio/pcm_utils.cc
    src/inferencing/generative/embeddings/embeddings_session.cc
    src/inferencing/generative/chat/chat_generator.cc
    src/inferencing/session/session.cc
    src/inferencing/session/session_manager.cc
    src/inferencing/generative/chat/chat_session.cc
    src/inferencing/generative/chat/chat_template.cc
    src/configuration.cc
    src/download/blob_download_state.cc
    src/download/blob_downloader.cc
    src/platform/cross_process_file_lock.cc
    src/download/download_manager.cc
    src/download/file_writer.cc
    src/download/inference_model_writer.cc
    src/download/model_registry_client.cc
    src/ep_detection/cuda_ep_bootstrapper.cc
    src/ep_detection/ep_detector.cc
    src/ep_detection/ep_utils.cc
    src/ep_detection/runtime_version_info.cc
    src/ep_detection/webgpu_ep_bootstrapper.cc
    src/exception.cc
    src/inferencing/generative/genai_config.cc
    src/http/http_client.cc
    src/inferencing/generative/genai_model_instance.cc
    src/logger.cc
    src/spdlog_logger.cc
    src/model_info.cc
    src/inferencing/model_load_manager.cc
    src/inferencing/generative/chat/onnx_chat_generator.cc
    src/model.cc
    src/inferencing/generative/openresponses/response_converter.cc
    src/inferencing/generative/openresponses/response_store.cc
    src/contracts/responses_json.cc
    src/inferencing/generative/toolcalling/tool_call_utils.cc
    src/inferencing/generative/toolcalling/grammar.cc
    src/inferencing/generative/chat/search_options.cc
    src/inferencing/predictive/inference_session.cc
    src/inferencing/predictive/tensor.cc
    src/manager.cc
    src/contracts/chat_completions.cc
    src/contracts/chat_completions_converter.cc
    src/contracts/audio_transcriptions.cc
    src/contracts/embeddings.cc
    src/service/audio_transcriptions_handler.cc
    src/service/embeddings_handler.cc
    src/service/chat_completions_handler.cc
    src/service/models_handlers.cc
    src/service/responses_handler.cc
    src/service/web_service.cc
    src/telemetry/telemetry.cc
    src/telemetry/telemetry_action_tracker.cc
    src/telemetry/telemetry_logger.cc
    src/utils.cc
    src/util/file_lock.cc
    src/http/http_download.cc
    src/util/path_safety.cc
    src/util/region_fallback.cc
    src/util/sha256.cc
    src/util/zip_extract.cc
    ${FOUNDRY_LOCAL_PLATFORM_SOURCES}
    ${FOUNDRY_LOCAL_INTERNAL_HEADERS}
)

# Organize headers into filters matching the directory structure in Visual Studio
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source" FILES ${FOUNDRY_LOCAL_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Public Headers" FILES ${FOUNDRY_LOCAL_PUBLIC_HEADERS})

list(APPEND FOUNDRY_LOCAL_SOURCES ${FOUNDRY_LOCAL_PUBLIC_HEADERS})

# --------------------------------------------------------------------------
# Common configuration applied to both shared and static library targets.
# LINK_SCOPE controls dependency propagation:
#   PRIVATE for the shared lib (deps are encapsulated behind the DLL boundary)
#   PUBLIC  for the static lib (consumers must link the transitive deps)
# --------------------------------------------------------------------------
function(foundry_local_configure_target TARGET LINK_SCOPE)
    target_compile_options(${TARGET} PRIVATE ${FOUNDRY_LOCAL_COMPILE_OPTIONS})

    target_include_directories(${TARGET}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    target_link_libraries(${TARGET}
        ${LINK_SCOPE}
            nlohmann_json::nlohmann_json
            Azure::azure-core
            Azure::azure-storage-blobs
            spdlog::spdlog
    )

    if(TARGET OnnxRuntimeGenAI::OnnxRuntimeGenAI)
        target_link_libraries(${TARGET} ${LINK_SCOPE}
            OnnxRuntimeGenAI::OnnxRuntimeGenAI
            OnnxRuntime::OnnxRuntime
        )
    else()
        target_compile_definitions(${TARGET} ${LINK_SCOPE} FOUNDRY_LOCAL_NO_ORT=1)
    endif()

    if(WIN32)
        target_link_libraries(${TARGET} ${LINK_SCOPE} dbghelp bcrypt)
        # UWP builds have CMAKE_SYSTEM_NAME = "WindowsStore"; desktop = "Windows".
        # WinHTTP is not available in UWP, so only define this on desktop Windows.
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
            target_compile_definitions(${TARGET} PRIVATE FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT=1)
        endif()
    endif()

    if(ANDROID)
        target_link_libraries(${TARGET} ${LINK_SCOPE} log)
    endif()

    if(FOUNDRY_LOCAL_BUILD_SERVICE)
        target_link_libraries(${TARGET} ${LINK_SCOPE} oatpp::oatpp)
        target_compile_definitions(${TARGET} ${LINK_SCOPE} FOUNDRY_LOCAL_HAS_WEB_SERVICE=1)
    endif()

    if(WinMLEpCatalog_FOUND)
        target_link_libraries(${TARGET} ${LINK_SCOPE} WinMLEpCatalog::WinMLEpCatalog)
        target_compile_definitions(${TARGET} ${LINK_SCOPE} FOUNDRY_LOCAL_HAS_EP_CATALOG=1)
    else()
        target_compile_definitions(${TARGET} ${LINK_SCOPE} FOUNDRY_LOCAL_HAS_EP_CATALOG=0)
    endif()
endfunction()

# --------------------------------------------------------------------------
# Generate version header from CMake project version (single source of truth)
# --------------------------------------------------------------------------
# CI overrides via -DFOUNDRY_LOCAL_VERSION_STRING=<full-version-string> so the
# binary's FoundryLocalGetVersionString() matches the .nupkg version (which
# typically includes a dev/timestamp suffix). Defaults to PROJECT_VERSION for
# local developer builds.
set(FOUNDRY_LOCAL_VERSION_STRING "${PROJECT_VERSION}"
    CACHE STRING "Version string returned by FoundryLocalGetVersionString()")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY
)

# --------------------------------------------------------------------------
# Object library — compiles all sources once. Both the shared (DLL) and
# static library targets re-use these object files, avoiding a double build.
# --------------------------------------------------------------------------
add_library(foundry_local_objects OBJECT ${FOUNDRY_LOCAL_SOURCES})
set_target_properties(foundry_local_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
foundry_local_configure_target(foundry_local_objects PUBLIC)
# FL_STATIC_LIBRARY makes FL_EXPORT empty on Windows so the two exported entry
# points compile without __declspec(dllimport/dllexport). The DLL gets its
# exports from the .def file instead.
target_compile_definitions(foundry_local_objects PRIVATE FL_STATIC_LIBRARY=1)

# --------------------------------------------------------------------------
# Shared library — for releases and public API consumers.
# On Windows a .def file exports the two public entry points, replacing
# __declspec(dllexport) so the objects can be compiled once.
# --------------------------------------------------------------------------
add_library(foundry_local SHARED)
target_link_libraries(foundry_local PRIVATE foundry_local_objects)

# --------------------------------------------------------------------------
# RPATH — bake "look beside me" into the shared library so it works in every
# deployment shape without per-consumer patching.
#
# Every layout we ship — C++ build tree, Python wheel _native/<rid>/, future
# Rust/Go bindings — drops libfoundry_local's runtime dependencies (ORT,
# GenAI, vcpkg shared deps) directly next to the library. So $ORIGIN (Linux)
# / @loader_path (macOS) is the universally correct answer, and baking it
# here means no downstream package needs to run patchelf or install_name_tool.
#
# This MIRRORS the FLC NativeAOT pattern: their .NET-published .so/.dylib
# also has $ORIGIN/@loader_path baked in by their toolchain, and the same
# binary works for both their build tree and their NuGet+wheel layouts.
#
# Important: BUILD_RPATH is APPENDED to the rpath entries CMake auto-derives
# from link-library directories, NOT replacing them. So dependents linking
# against build-tree foundry_local still resolve their transitive NEEDED
# (libonnxruntime.so.1 etc.) via the FetchContent _deps paths.
#
# We do NOT set BUILD_WITH_INSTALL_RPATH — leave it FALSE. Setting it TRUE
# strips the auto-derived link-dir rpath from the build-tree binary and
# breaks link-time resolution for dependents (cache_only_tests etc.).
if(APPLE)
    set_target_properties(foundry_local PROPERTIES
        BUILD_RPATH   "@loader_path"
        INSTALL_RPATH "@loader_path"
    )
elseif(UNIX)
    set_target_properties(foundry_local PROPERTIES
        BUILD_RPATH   "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

if(WIN32)
    target_sources(foundry_local PRIVATE src/exports.def)

    # Delay-load the WinML EP Catalog DLL — it may not be present on older Windows.
    # The bootstrapper handles the delay-load failure gracefully at runtime.
    if(WinMLEpCatalog_FOUND)
        target_link_options(foundry_local PRIVATE
            /DELAYLOAD:Microsoft.Windows.AI.MachineLearning.dll
        )
    endif()
endif()

# --------------------------------------------------------------------------
# Static library — for internal/low-level tests that access private symbols.
# --------------------------------------------------------------------------
add_library(foundry_local_static STATIC)
target_link_libraries(foundry_local_static PUBLIC foundry_local_objects)
target_compile_definitions(foundry_local_static PUBLIC FL_STATIC_LIBRARY=1)

# --------------------------------------------------------------------------
# Copy ORT and ORT GenAI runtime libraries to the output directory.
# Without this, the system onnxruntime (if any) gets loaded instead,
# causing API version mismatches at runtime.
#
# Co-location is the zero-configuration default: language bindings are
# responsible for preloading ORT/GenAI from a non-default location when needed
# (see C# DllLoader.PreloadOrtIfPresent and Python prepare_native_dependencies).
#
# On Linux, GenAI does dlopen("libonnxruntime.so") internally. RUNPATH
# (the modern default) doesn't propagate to transitive dlopen, so the
# .so files must be co-located with the consuming binaries.
# --------------------------------------------------------------------------
if(TARGET OnnxRuntime::OnnxRuntime)
    if(WIN32)
        add_custom_command(TARGET foundry_local POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_GENAI_LIB_DIR}/onnxruntime-genai.dll"
                $<TARGET_FILE_DIR:foundry_local>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_DLL_DIR}/onnxruntime.dll"
                $<TARGET_FILE_DIR:foundry_local>
        )

        if(EXISTS "${ORT_DLL_DIR}/onnxruntime_providers_shared.dll")
            add_custom_command(TARGET foundry_local POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ORT_DLL_DIR}/onnxruntime_providers_shared.dll"
                    $<TARGET_FILE_DIR:foundry_local>
            )
        endif()

        # Copy WinML EP Catalog DLL for runtime EP discovery
        if(WinMLEpCatalog_FOUND AND EXISTS "${WINML_EP_CATALOG_DLL_DIR}/Microsoft.Windows.AI.MachineLearning.dll")
            add_custom_command(TARGET foundry_local POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WINML_EP_CATALOG_DLL_DIR}/Microsoft.Windows.AI.MachineLearning.dll"
                    $<TARGET_FILE_DIR:foundry_local>
            )
        endif()
    elseif(APPLE)
        # macOS: copy dylibs so consumers that only link libfoundry_local.dylib (e.g. cache_only_tests) find the correct
        # ORT version instead of any system-installed ORT, which would cause an Ort::InitApi() version mismatch.
        #
        # The ORT nupkg ships the library as libonnxruntime.dylib with install_name @rpath/libonnxruntime.<major>.dylib
        # (the major version is the soname; e.g. libonnxruntime.1.dylib). foundry_local records that soname as its ORT
        # dependency, while GenAI dlopen()s the unversioned libonnxruntime.dylib at runtime — so we keep the shipped file
        # under its own name and add a soname symlink beside it. Mirrors the Linux libonnxruntime.so / .so.1 layout below.
        string(REGEX MATCH "^[0-9]+" ORT_VERSION_MAJOR "${ORT_VERSION}")
        add_custom_command(TARGET foundry_local POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_GENAI_LIB_DIR}/libonnxruntime-genai.dylib"
                $<TARGET_FILE_DIR:foundry_local>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_LIB_DIR}/libonnxruntime.dylib"
                $<TARGET_FILE_DIR:foundry_local>
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                libonnxruntime.dylib
                $<TARGET_FILE_DIR:foundry_local>/libonnxruntime.${ORT_VERSION_MAJOR}.dylib
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        # Linux and Android both use .so files
        add_custom_command(TARGET foundry_local POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_GENAI_LIB_DIR}/libonnxruntime-genai.so"
                $<TARGET_FILE_DIR:foundry_local>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_LIB_DIR}/libonnxruntime.so"
                $<TARGET_FILE_DIR:foundry_local>
        )

        if(EXISTS "${ORT_LIB_DIR}/libonnxruntime_providers_shared.so")
            add_custom_command(TARGET foundry_local POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ORT_LIB_DIR}/libonnxruntime_providers_shared.so"
                    $<TARGET_FILE_DIR:foundry_local>
            )
        endif()
    endif()
endif()

# --------------------------------------------------------------------------
# Header-only C++ wrapper for external consumers
# --------------------------------------------------------------------------
add_library(foundry_local_cpp INTERFACE)
target_include_directories(foundry_local_cpp
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(foundry_local_cpp INTERFACE foundry_local Microsoft.GSL::GSL)

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(FOUNDRY_LOCAL_BUILD_TESTS)
    add_subdirectory(test)
    set_target_properties(foundry_local_tests PROPERTIES FOLDER "Tests")
    set_target_properties(sdk_integration_tests PROPERTIES FOLDER "Tests")
endif()

# --------------------------------------------------------------------------
# Examples
# --------------------------------------------------------------------------
if(FOUNDRY_LOCAL_BUILD_EXAMPLES)
    add_subdirectory(examples/basic_chat)
    add_subdirectory(examples/tool_calling)
    add_subdirectory(examples/realtime_audio)
    add_subdirectory(examples/embeddings)
    set_target_properties(basic_chat_example PROPERTIES FOLDER "Examples")
    set_target_properties(tool_calling_example PROPERTIES FOLDER "Examples")
    set_target_properties(realtime_audio_example PROPERTIES FOLDER "Examples")
    set_target_properties(embeddings_example PROPERTIES FOLDER "Examples")

    # On Linux, GenAI does dlopen("libonnxruntime.so") internally. Modern GCC
    # emits RUNPATH which doesn't propagate to transitive dlopen calls.
    # --disable-new-dtags forces the older RPATH tag, which does propagate.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(basic_chat_example PRIVATE -Wl,--disable-new-dtags)
        target_link_options(tool_calling_example PRIVATE -Wl,--disable-new-dtags)
        target_link_options(realtime_audio_example PRIVATE -Wl,--disable-new-dtags)
        target_link_options(embeddings_example PRIVATE -Wl,--disable-new-dtags)
    endif()
endif()
