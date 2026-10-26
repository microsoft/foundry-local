# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

if(CMAKE_CROSSCOMPILING OR ANDROID OR IOS)
    return()
endif()
set(TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR "" CACHE PATH
    "Matching 1DS source tree for deterministic shutdown regressions with the static CONFIG package")
if(NOT TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR)
    message(STATUS "Telemetry shutdown regression requires TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR")
    return()
endif()

set(shutdown_sdk_dir "${CMAKE_CURRENT_BINARY_DIR}/telemetry_shutdown_sdk")
file(MAKE_DIRECTORY "${shutdown_sdk_dir}")
foreach(source callbacks/DebugSource.cpp pal/WorkerThread.cpp api/Logger.cpp pal/PAL.cpp http/HttpRequestEncoder.cpp)
    set(source_path "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_path}")
    file(READ "${source_path}" instrumented)
    if(source STREQUAL "callbacks/DebugSource.cpp")
        string(REGEX MATCHALL "DE_LOCKGUARD\\(stateLock\\(\\)\\)" sites "${instrumented}")
        list(LENGTH sites count)
        if(NOT count EQUAL 5)
            message(FATAL_ERROR "Update shutdown regression debug-lock instrumentation")
        endif()
        string(REPLACE "DE_LOCKGUARD(stateLock());"
            "TelemetryTestBeforeDebugLock();\n        (void)stateLock();\n        TelemetryTestDebugLockInitialized();\n        DE_LOCKGUARD(stateLock());"
            instrumented "${instrumented}")
        string(PREPEND instrumented
            "extern \"C\" void TelemetryTestBeforeDebugLock();\nextern \"C\" void TelemetryTestDebugLockInitialized();\n")
    elseif(source STREQUAL "pal/WorkerThread.cpp")
        string(REGEX MATCHALL "threadToJoin\\.join\\(\\)" sites "${instrumented}")
        list(LENGTH sites count)
        if(NOT count EQUAL 2)
            message(FATAL_ERROR "Update shutdown regression native-join instrumentation")
        endif()
        string(REPLACE "threadToJoin.join();"
            "threadToJoin.join();\n                    TelemetryTestWorkerJoined(this);"
            instrumented "${instrumented}")
        set(start_site "m_hThread = std::thread(WorkerThread::threadFunc, static_cast<void*>(this));")
        string(FIND "${instrumented}" "${start_site}" start_position)
        if(start_position EQUAL -1)
            message(FATAL_ERROR "Update shutdown regression worker-start instrumentation")
        endif()
        string(REPLACE "${start_site}" "TelemetryTestWorkerStarted(this);\n            ${start_site}"
            instrumented "${instrumented}")
        string(PREPEND instrumented
            "extern \"C\" void TelemetryTestWorkerStarted(void*);\nextern \"C\" void TelemetryTestWorkerJoined(void*);\n")
    elseif(source STREQUAL "api/Logger.cpp")
        # Observe, but do not bypass, production decoration, queuing, persistence, or encoding.
        set(log_site "void Logger::LogEvent(EventProperties const& properties)\n    {")
        string(FIND "${instrumented}" "${log_site}" log_position)
        if(log_position EQUAL -1)
            message(FATAL_ERROR "Update shutdown regression collector-event interception")
        endif()
        string(REPLACE "${log_site}"
            "${log_site}\n        TelemetryTestCollectorEvent(properties.GetName().c_str());"
            instrumented "${instrumented}")
        string(PREPEND instrumented "extern \"C\" void TelemetryTestCollectorEvent(const char*);\n")
    elseif(source STREQUAL "pal/PAL.cpp")
        # Track actual string destruction in the unpatched negative control as well as the fixed SDK.
        string(REPLACE "using SdkVersion = std::string;" "using SdkVersion = TelemetryTestSdkVersion;"
            instrumented "${instrumented}")
        string(REPLACE "static const std::string sdkVersion(" "static const TelemetryTestSdkVersion sdkVersion("
            instrumented "${instrumented}")
        string(REPLACE "return *sdkVersion;" "TelemetryTestVersionAccess();\n        return *sdkVersion;"
            instrumented "${instrumented}")
        string(REPLACE "return sdkVersion;" "TelemetryTestVersionAccess();\n        return sdkVersion;"
            instrumented "${instrumented}")
        string(REGEX MATCHALL "TelemetryTestVersionAccess\\(\\)" sites "${instrumented}")
        list(LENGTH sites count)
        if(NOT count EQUAL 1)
            message(FATAL_ERROR "Update shutdown regression SDK-version lifetime instrumentation")
        endif()
        string(PREPEND instrumented [=[
#include <string>
extern "C" void TelemetryTestVersionDestroyed();
extern "C" void TelemetryTestVersionAccess();
struct TelemetryTestSdkVersion : std::string {
    using std::string::string;
    ~TelemetryTestSdkVersion() { TelemetryTestVersionDestroyed(); }
};
]=])
    else()
        set(encode_site "bool HttpRequestEncoder::handleEncode(EventsUploadContextPtr const& ctx)\n    {")
        string(FIND "${instrumented}" "${encode_site}" encode_position)
        if(encode_position EQUAL -1)
            message(FATAL_ERROR "Update shutdown regression encoder instrumentation")
        endif()
        string(REPLACE "${encode_site}" "${encode_site}\n        TelemetryTestBeforeEncode();"
            instrumented "${instrumented}")
        string(PREPEND instrumented "extern \"C\" void TelemetryTestBeforeEncode();\n")
    endif()
    get_filename_component(name "${source}" NAME)
    file(WRITE "${shutdown_sdk_dir}/${name}" "${instrumented}")
endforeach()

if(WIN32 AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_library(windows-telemetry-shutdown-fixture SHARED internal_api/windows-telemetry-shutdown-library.cc)
    target_link_libraries(windows-telemetry-shutdown-fixture PRIVATE foundry_local_static MSTelemetry::mat)
    target_compile_options(windows-telemetry-shutdown-fixture PRIVATE ${FOUNDRY_LOCAL_COMPILE_OPTIONS})
    target_include_directories(windows-telemetry-shutdown-fixture PRIVATE ../src)
    if(MSVC)
        target_link_options(windows-telemetry-shutdown-fixture PRIVATE /guard:cf /DYNAMICBASE)
    endif()
    add_dependencies(windows-telemetry-shutdown-fixture foundry_local)
    add_executable(windows-telemetry-shutdown-test windows-telemetry-shutdown-test.cc)
    target_compile_options(windows-telemetry-shutdown-test PRIVATE ${FOUNDRY_LOCAL_COMPILE_OPTIONS})
    if(MSVC)
        target_link_options(windows-telemetry-shutdown-test PRIVATE /guard:cf /DYNAMICBASE)
    endif()
    add_dependencies(windows-telemetry-shutdown-test windows-telemetry-shutdown-fixture)
    foreach(component manager telemetry)
        set(modes process-exit normal-destruction)
        # Active Manager runtime resources can retain the DLL; the standalone bridge tests actual FreeLibrary teardown.
        if(component STREQUAL "telemetry")
            list(APPEND modes explicit-unload)
        endif()
        foreach(mode IN LISTS modes)
            set(name "WindowsTelemetryShutdown-${component}-${mode}")
            set(home "${CMAKE_CURRENT_BINARY_DIR}/${name}")
            file(MAKE_DIRECTORY "${home}")
            add_test(NAME "${name}" COMMAND ${CMAKE_COMMAND} -E env
                "HOME=${home}" "XDG_CACHE_HOME=${home}" "APPDATA=${home}" "LOCALAPPDATA=${home}"
                "TELEMETRY_TEST_HOME=${home}" CI=0 TF_BUILD=0 GITHUB_ACTIONS=0 GITLAB_CI=0 CIRCLECI=0 TRAVIS=0
                JENKINS_URL=0 CODEBUILD_BUILD_ID=0 BUILDKITE=0 TEAMCITY_VERSION=0 APPVEYOR=0
                BITBUCKET_BUILD_NUMBER=0 SYSTEM_TEAMFOUNDATIONCOLLECTIONURI=0 ORT_RUNNING_UNIT_TESTS=0
                ORT_DISABLE_TELEMETRY=1 ORT_TELEMETRY_DISABLED=0
                $<TARGET_FILE:windows-telemetry-shutdown-test> $<TARGET_FILE:windows-telemetry-shutdown-fixture>
                "${component}" "${mode}")
            set_tests_properties("${name}" PROPERTIES TIMEOUT 20)
        endforeach()
    endforeach()
endif()

add_library(telemetry_shutdown_sdk OBJECT
    "${shutdown_sdk_dir}/DebugSource.cpp" "${shutdown_sdk_dir}/WorkerThread.cpp" "${shutdown_sdk_dir}/Logger.cpp"
    "${shutdown_sdk_dir}/PAL.cpp" "${shutdown_sdk_dir}/HttpRequestEncoder.cpp")
target_link_libraries(telemetry_shutdown_sdk PRIVATE MSTelemetry::mat)
target_compile_definitions(telemetry_shutdown_sdk PRIVATE MATSDK_DISABLE_LOGGING MATSDK_DISABLE_DEVICE_ID)
target_include_directories(telemetry_shutdown_sdk PRIVATE
    "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib" "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/api"
    "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/pal" "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/http"
    "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/include"
    "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/include/mat")
add_executable(telemetry_shutdown_tests internal_api/telemetry-shutdown-tests.cc
    $<TARGET_OBJECTS:telemetry_shutdown_sdk>)
target_link_libraries(telemetry_shutdown_tests PRIVATE foundry_local_static MSTelemetry::mat)
target_compile_options(telemetry_shutdown_tests PRIVATE ${FOUNDRY_LOCAL_COMPILE_OPTIONS})
target_include_directories(telemetry_shutdown_tests PRIVATE ../src
    "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib" "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/include"
    "${TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR}/lib/include/mat")
add_dependencies(telemetry_shutdown_tests foundry_local)
if(MSVC)
    target_link_options(telemetry_shutdown_tests PRIVATE /guard:cf /DYNAMICBASE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(telemetry_shutdown_tests PRIVATE "LINKER:--disable-new-dtags" "LINKER:--exclude-libs,ALL")
endif()

foreach(mode retained threaded recreated explicit prime-only optout ci-disabled enabled-after-ci late-create upload-after-ci)
    set(home "${CMAKE_CURRENT_BINARY_DIR}/telemetry-shutdown-${mode}")
    file(MAKE_DIRECTORY "${home}")
    set(ci 0)
    set(optout 0)
    if(mode STREQUAL "ci-disabled" OR mode STREQUAL "enabled-after-ci" OR mode STREQUAL "upload-after-ci")
        set(ci 1)
    elseif(mode STREQUAL "optout")
        set(optout 1)
    endif()
    add_test(NAME TelemetryShutdown-${mode}
        COMMAND ${CMAKE_COMMAND} -E env
            "HOME=${home}" "XDG_CACHE_HOME=${home}" "APPDATA=${home}" "LOCALAPPDATA=${home}"
            "TELEMETRY_TEST_HOME=${home}" "CI=${ci}" TF_BUILD=0 GITHUB_ACTIONS=0 GITLAB_CI=0 CIRCLECI=0 TRAVIS=0
            JENKINS_URL=0 CODEBUILD_BUILD_ID=0 BUILDKITE=0 TEAMCITY_VERSION=0 APPVEYOR=0
            BITBUCKET_BUILD_NUMBER=0 SYSTEM_TEAMFOUNDATIONCOLLECTIONURI=0 ORT_RUNNING_UNIT_TESTS=0
            ORT_DISABLE_TELEMETRY=1 "ORT_TELEMETRY_DISABLED=${optout}"
            $<TARGET_FILE:telemetry_shutdown_tests> ${mode})
    set_tests_properties(TelemetryShutdown-${mode} PROPERTIES TIMEOUT 60)
endforeach()
