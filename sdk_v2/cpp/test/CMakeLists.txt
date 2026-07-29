# Copyright (c) Microsoft. All rights reserved.

find_package(httplib CONFIG REQUIRED)

# ==========================================================================
# Internal tests — test the implementation directly
# Links against the static library with access to internal src/ headers.
# ==========================================================================
add_executable(foundry_local_tests
    internal_api/audio/audio_session_test.cc
    internal_api/audio/audio_transcription_contract_test.cc
    internal_api/audio/pcm_utils_test.cc
    internal_api/base_model_catalog_test.cc
    internal_api/blob_download_state_test.cc
    internal_api/c_api_test.cc
    internal_api/callback_handler_test.cc
    internal_api/catalog_cache_test.cc
    internal_api/chat/chat_generator_test.cc
    internal_api/chat/chat_session_test.cc
    internal_api/chat/chat_template_test.cc
    internal_api/chat/onnx_chat_generator_vision_test.cc
    internal_api/chat_completions_test.cc
    internal_api/chat_completions_converter_test.cc
    internal_api/configuration_test.cc
    internal_api/cross_process_file_lock_test.cc
    internal_api/download_test.cc
    internal_api/embeddings/contracts_embeddings_test.cc
    internal_api/embeddings/fp16_test.cc
    internal_api/ep_detector_test.cc
    internal_api/exception_test.cc
    internal_api/execution_provider_test.cc
    internal_api/file_lock_test.cc
    internal_api/file_uri_test.cc
    internal_api/file_writer_test.cc
    internal_api/genai_config_test.cc
    internal_api/http_download_test.cc
    internal_api/http_retry_test.cc
    internal_api/item_test.cc
    internal_api/local_model_scanner_test.cc
    internal_api/model_info_test.cc
    internal_api/model_info_accessors_test.cc
    internal_api/model_io_info_test.cc
    internal_api/model_load_manager_test.cc
    internal_api/model_sorting_test.cc
    internal_api/platform_path_test.cc
    internal_api/response_converter_test.cc
    internal_api/response_store_test.cc
    internal_api/responses_json_test.cc
    internal_api/session_manager_test.cc
    internal_api/sha256_test.cc
    internal_api/region_fallback_test.cc
    internal_api/sse_stream_body_test.cc
    internal_api/azure_catalog_test.cc
    internal_api/telemetry_test.cc
    internal_api/tensor_test.cc
    internal_api/chat/search_options_test.cc
    internal_api/toolcalling/tool_call_stream_accumulator_test.cc
    internal_api/toolcalling/tool_call_utils_test.cc
    internal_api/toolcalling/grammar_test.cc
    internal_api/utils_test.cc
    internal_api/web_service_test.cc
    internal_api/zip_extract_test.cc
)

target_compile_options(foundry_local_tests PRIVATE ${FOUNDRY_LOCAL_COMPILE_OPTIONS})

target_link_libraries(foundry_local_tests
    PRIVATE
        foundry_local_static
        GTest::gtest_main
        httplib::httplib
)

# On Linux, GenAI does dlopen("libonnxruntime.so") internally. Modern GCC emits
# RUNPATH by default, which doesn't propagate to transitive dlopen calls.
# --disable-new-dtags forces the older RPATH tag, which does propagate.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(foundry_local_tests PRIVATE -Wl,--disable-new-dtags)
endif()

# Ensure ORT/GenAI DLLs are copied to the bin directory before test discovery.
# The shared library target has POST_BUILD commands that copy the runtime DLLs.
add_dependencies(foundry_local_tests foundry_local)

# Internal headers (src/) needed by configuration_test.cc
target_include_directories(foundry_local_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_command(TARGET foundry_local_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata
            $<TARGET_FILE_DIR:foundry_local_tests>/testdata
    # Copy shared cross-SDK test data (audio, images, etc.).
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/../../testdata
            $<TARGET_FILE_DIR:foundry_local_tests>/testdata
)

# Make the build output testdata path available to test code at compile time.
# On Android the binary runs from the directory containing testdata/, so a relative path works.
if(ANDROID)
    target_compile_definitions(foundry_local_tests PRIVATE FOUNDRY_LOCAL_TEST_DATA_DIR="testdata")
else()
    target_compile_definitions(foundry_local_tests PRIVATE
        FOUNDRY_LOCAL_TEST_DATA_DIR="$<TARGET_FILE_DIR:foundry_local_tests>/testdata"
    )
endif()

include(GoogleTest)
# DISCOVERY_TIMEOUT defaults to 5s, which is not enough for the Windows build:
# foundry_local.dll links against Microsoft.Windows.AI.MachineLearning.dll
# (delay-loaded) plus standalone ORT, and process startup + gtest enumeration
# can exceed 5s on cold caches or slow CI agents. Bump to 60s for headroom.
gtest_discover_tests(foundry_local_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:foundry_local_tests>
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
)

# ==========================================================================
# SDK API tests — test the public C++ API surface only
# Links the shared library via foundry_local_cpp (the INTERFACE library for
# external consumers). No access to internal headers.
# ==========================================================================

add_executable(sdk_integration_tests
    sdk_api/audio_transcriptions_test.cc
    sdk_api/cpp_api_test.cc
    sdk_api/catalog_test.cc
    sdk_api/download_test.cc
    sdk_api/embeddings_test.cc
    sdk_api/ep_detection_test.cc
    sdk_api/chat_session_test.cc
    sdk_api/model_test.cc
    sdk_api/model_endpoints_test.cc
    sdk_api/chat_completions_test.cc
    sdk_api/chat_completions_from_json.cc
    sdk_api/responses_test.cc
    sdk_api/responses_vision_test.cc
    sdk_api/vision_session_test.cc
    sdk_api/streaming_audio_test.cc
    sdk_api/reasoning_model_test.cc
)

# Public API header targets C++17 — build this test as C++17 to verify compatibility.
set_target_properties(sdk_integration_tests PROPERTIES CXX_STANDARD 17)
target_compile_options(sdk_integration_tests PRIVATE ${FOUNDRY_LOCAL_COMPILE_OPTIONS})

# sdk_api/ contains model_fixture.h and other shared headers.
# src/contracts/ provides the struct-only contract headers (chat_completions.h, etc.).
# src/util/ provides json_helpers.h (header-only) for the test-side from_json implementations.
target_include_directories(sdk_integration_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sdk_api
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/contracts
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/util
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sdk_integration_tests
    PRIVATE
        foundry_local_cpp
        GTest::gtest_main
        nlohmann_json::nlohmann_json
        httplib::httplib
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(sdk_integration_tests PRIVATE -Wl,--disable-new-dtags)
endif()

add_custom_command(TARGET sdk_integration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata
            $<TARGET_FILE_DIR:sdk_integration_tests>/testdata
    # Copy shared cross-SDK test data (audio, images, etc.).
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/../../testdata
            $<TARGET_FILE_DIR:sdk_integration_tests>/testdata
)

if(ANDROID)
    target_compile_definitions(sdk_integration_tests PRIVATE FOUNDRY_LOCAL_TEST_DATA_DIR="testdata")
else()
    target_compile_definitions(sdk_integration_tests PRIVATE
        FOUNDRY_LOCAL_TEST_DATA_DIR="$<TARGET_FILE_DIR:sdk_integration_tests>/testdata"
    )
endif()

# Register as a single CTest entry — NOT per-test discovery.
# SharedTestEnv performs expensive one-time setup (CUDA EP registration,
# catalog fetch, model download+load) that must happen once per process.
# gtest_discover_tests would launch a separate process per test, repeating
# that setup dozens of times and causing port conflicts between concurrent
# web service instances.
add_test(
    NAME sdk_integration_tests
    COMMAND sdk_integration_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:sdk_integration_tests>
)
# The integration suite loads models in SharedTestEnv (slow on CPU) and runs
# dozens of inference tests. The default CTest --timeout (600s) is not enough.
set_tests_properties(sdk_integration_tests PROPERTIES TIMEOUT 1200)

# ==========================================================================
# Cache-only mode tests — tests external_service_url / cache-only catalog
# Separate binary because Manager is a singleton and these tests create
# their own Manager instances with different configurations.
# ==========================================================================
add_executable(cache_only_tests
    sdk_api/cache_only_test.cc
    sdk_api/catalog_live_test.cc
    sdk_api/manager_web_service_test.cc
)

set_target_properties(cache_only_tests PROPERTIES CXX_STANDARD 17)
target_compile_options(cache_only_tests PRIVATE ${FOUNDRY_LOCAL_COMPILE_OPTIONS})

# Pick up the shared fl::test::SafeGetEnv helper.
target_include_directories(cache_only_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(cache_only_tests
    PRIVATE
        foundry_local_cpp
        GTest::gtest_main
        nlohmann_json::nlohmann_json
)

add_dependencies(cache_only_tests foundry_local)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(cache_only_tests PRIVATE -Wl,--disable-new-dtags)
endif()

# On macOS, libonnxruntime-genai.dylib has no RPATH of its own and no explicit
# libonnxruntime dependency. It resolves OrtGetApiBase via flat-namespace lookup,
# which picks up the first libonnxruntime in the process. Without these RPATH entries
# a system-installed ORT (different version) can be loaded first, causing
# Ort::InitApi() to throw an API version mismatch at dyld initialization time.
if(APPLE)
    set_target_properties(cache_only_tests PROPERTIES
        BUILD_RPATH "${ORT_GENAI_LIB_DIR};${ORT_LIB_DIR}"
    )
endif()

gtest_discover_tests(cache_only_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:cache_only_tests>
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
)
