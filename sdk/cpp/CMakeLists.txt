cmake_minimum_required(VERSION 3.20)

# VS hot reload policy (safe-guarded)
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  if (MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>")
  endif()
endif()

project(CppSdk LANGUAGES CXX)

# -----------------------------
# Compiler guard
# -----------------------------
if (WIN32)
  # Accept MSVC OR clang-cl (Clang in MSVC compatibility mode).
  if (NOT (MSVC OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")))
    message(STATUS "CMAKE_CXX_COMPILER_ID = ${CMAKE_CXX_COMPILER_ID}")
    message(STATUS "CMAKE_CXX_COMPILER    = ${CMAKE_CXX_COMPILER}")
    message(STATUS "CMAKE_CXX_SIMULATE_ID = ${CMAKE_CXX_SIMULATE_ID}")
    message(FATAL_ERROR "On Windows, need MSVC or clang-cl (MSVC-compatible toolchain).")
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (WIN32)
  # Target Windows 10+ APIs
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# -----------------------------
# Dependencies (installed via vcpkg)
# -----------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)
option(BUILD_TESTING "Build unit and end-to-end tests" ON)
if (BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
endif()

# -----------------------------
# SDK library (STATIC)
# List ONLY .cpp files here.
# -----------------------------
add_library(CppSdk STATIC
  src/model.cpp
  src/catalog.cpp
  src/chat_client.cpp
  src/audio_client.cpp
  src/live_audio_types.cpp
  src/live_audio_session.cpp
  src/foundry_local_manager.cpp
)

target_include_directories(CppSdk
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(CppSdk
  PUBLIC
    nlohmann_json::nlohmann_json
    Microsoft.GSL::GSL
)

if (UNIX)
  target_link_libraries(CppSdk PUBLIC ${CMAKE_DL_LIBS})
endif()

# -----------------------------
# Native dependencies (NuGet) — Windows only
# Auto-download Core DLL and ONNX Runtime DLLs needed at runtime.
# Versions are kept in sync with deps_versions.json.
# -----------------------------
if(WIN32)

set(FL_CORE_VERSION "1.2.1" CACHE STRING "Microsoft.AI.Foundry.Local.Core NuGet version")
set(FL_ORT_VERSION "1.26.0" CACHE STRING "Microsoft.ML.OnnxRuntime.Foundry NuGet version")
set(FL_ORTGENAI_VERSION "0.14.1" CACHE STRING "Microsoft.ML.OnnxRuntimeGenAI.Foundry NuGet version")
set(FL_NATIVE_DEPS_DIR "${CMAKE_CURRENT_BINARY_DIR}/_native_deps")

function(fl_ensure_nuget_package PKG_NAME PKG_VERSION)
    set(PKG_DIR "${FL_NATIVE_DEPS_DIR}/${PKG_NAME}.${PKG_VERSION}")
    if(NOT EXISTS "${PKG_DIR}")
        message(STATUS "Downloading ${PKG_NAME} ${PKG_VERSION}...")
        find_program(NUGET_EXE nuget)
        if(NOT NUGET_EXE)
            message(FATAL_ERROR "nuget.exe not found on PATH. Install NuGet CLI: https://www.nuget.org/downloads")
        endif()
        execute_process(
            COMMAND ${NUGET_EXE} install ${PKG_NAME} -Version ${PKG_VERSION}
                    -OutputDirectory ${FL_NATIVE_DEPS_DIR} -NonInteractive
            RESULT_VARIABLE result
        )
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Failed to download ${PKG_NAME} ${PKG_VERSION}")
        endif()
    endif()
endfunction()

fl_ensure_nuget_package("Microsoft.AI.Foundry.Local.Core" ${FL_CORE_VERSION})
fl_ensure_nuget_package("Microsoft.ML.OnnxRuntime.Foundry" ${FL_ORT_VERSION})
fl_ensure_nuget_package("Microsoft.ML.OnnxRuntimeGenAI.Foundry" ${FL_ORTGENAI_VERSION})

set(FL_CORE_DLL_DIR "${FL_NATIVE_DEPS_DIR}/Microsoft.AI.Foundry.Local.Core.${FL_CORE_VERSION}/runtimes/win-x64/native")
set(FL_ORT_DLL_DIR "${FL_NATIVE_DEPS_DIR}/Microsoft.ML.OnnxRuntime.Foundry.${FL_ORT_VERSION}/runtimes/win-x64/native")
set(FL_ORTGENAI_DLL_DIR "${FL_NATIVE_DEPS_DIR}/Microsoft.ML.OnnxRuntimeGenAI.Foundry.${FL_ORTGENAI_VERSION}/runtimes/win-x64/native")

# Helper function: copy runtime DLLs next to any target that links CppSdk
function(fl_copy_runtime_dlls TARGET_NAME)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FL_CORE_DLL_DIR}/Microsoft.AI.Foundry.Local.Core.dll"
            $<TARGET_FILE_DIR:${TARGET_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FL_ORT_DLL_DIR}"
            $<TARGET_FILE_DIR:${TARGET_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FL_ORTGENAI_DLL_DIR}"
            $<TARGET_FILE_DIR:${TARGET_NAME}>
        COMMENT "Copying Foundry Local runtime DLLs for ${TARGET_NAME}..."
    )
endfunction()

else() # Non-Windows: provide a no-op fl_copy_runtime_dlls

function(fl_copy_runtime_dlls TARGET_NAME)
    # No DLL copying needed on non-Windows platforms
endfunction()

endif() # WIN32

# -----------------------------
# Sample executable
# -----------------------------
add_executable(CppSdkSample
  sample/main.cpp
)

target_link_libraries(CppSdkSample PRIVATE CppSdk)

# Copy DLLs for the SDK sample
fl_copy_runtime_dlls(CppSdkSample)

# -----------------------------
# Vision sample (Responses API) — built if present
# -----------------------------
set(VISION_SAMPLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sample/web-server-responses-vision")

if(EXISTS "${VISION_SAMPLE_DIR}/main.cpp")
  find_package(CURL QUIET)
  if(CURL_FOUND)
    add_executable(WebServerResponsesVision
      ${VISION_SAMPLE_DIR}/main.cpp
      ${VISION_SAMPLE_DIR}/stb_impl.cpp
    )

    target_link_libraries(WebServerResponsesVision PRIVATE CppSdk CURL::libcurl)
    fl_copy_runtime_dlls(WebServerResponsesVision)
    message(STATUS "Vision sample: enabled")
  else()
    message(STATUS "Vision sample: disabled (curl not found — add 'curl' to vcpkg.json)")
  endif()
else()
  message(STATUS "Vision sample: not found")
endif()

# -----------------------------
# Unit tests
# -----------------------------
if (BUILD_TESTING)
  enable_testing()

  add_executable(CppSdkTests
    test/parser_and_types_test.cpp
    test/model_variant_test.cpp
    test/catalog_test.cpp
    test/client_test.cpp
    test/ep_test.cpp
  )

  target_include_directories(CppSdkTests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/test
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(CppSdkTests PRIVATE FL_TESTS)

  target_link_libraries(CppSdkTests
    PRIVATE
      CppSdk
      GTest::gtest_main
  )

  # Copy testdata files next to the test executable so file-based tests can find them.
  add_custom_command(TARGET CppSdkTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/test/testdata
      $<TARGET_FILE_DIR:CppSdkTests>/testdata
  )

  include(GoogleTest)
  gtest_discover_tests(CppSdkTests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:CppSdkTests>
  )

  # -----------------------------
  # End-to-end tests (separate executable, requires Core DLL)
  # Exercises the full public API against the real catalog.
  # Tests that need model download are DISABLED by default;
  # run with --gtest_also_run_disabled_tests locally.
  # -----------------------------
  add_executable(CppSdkE2ETests
    test/e2e_test.cpp
  )

  target_include_directories(CppSdkE2ETests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/test
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(CppSdkE2ETests
    PRIVATE
      CppSdk
      GTest::gtest_main
  )

  gtest_discover_tests(CppSdkE2ETests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:CppSdkE2ETests>
  )
endif()

# Make Visual Studio start/debug this target by default
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  PROPERTY VS_STARTUP_PROJECT CppSdkSample)

# -----------------------------
# Install — produces the redistributable SDK zip layout:
#   lib/CppSdk.lib
#   include/...
#   bin/*.dll  (runtime DLLs)
#   cmake/FoundryLocalConfig.cmake
# -----------------------------
install(TARGETS CppSdk
  ARCHIVE DESTINATION $<IF:$<CONFIG:Debug>,lib/debug,lib>
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)

# Install runtime DLLs into bin/ (Windows only)
if(WIN32)
  install(DIRECTORY "${FL_CORE_DLL_DIR}/" DESTINATION bin FILES_MATCHING PATTERN "*.dll")
  install(DIRECTORY "${FL_ORT_DLL_DIR}/" DESTINATION bin FILES_MATCHING PATTERN "*.dll")
  install(DIRECTORY "${FL_ORTGENAI_DLL_DIR}/" DESTINATION bin FILES_MATCHING PATTERN "*.dll")
endif()

# Install CMake config file
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FoundryLocalConfig.cmake DESTINATION cmake)

# Install vision sample into the zip
if(EXISTS "${VISION_SAMPLE_DIR}/main.cpp")
  install(DIRECTORY "${VISION_SAMPLE_DIR}/"
    DESTINATION sample/web-server-responses-vision
    PATTERN "build" EXCLUDE
    PATTERN "out" EXCLUDE
    PATTERN "_native_deps" EXCLUDE
    PATTERN "vcpkg_installed" EXCLUDE
  )
endif()
