# Copyright (c) Microsoft. All rights reserved.
# Find/acquire ONNX Runtime GenAI.
#
# All platforms / flavors: Microsoft.ML.OnnxRuntimeGenAI.Foundry
#
# When ORT_GENAI_HOME is set, uses the local ORT GenAI build instead of NuGet.
# Otherwise uses FetchContent from nuget.org.
# Creates an IMPORTED target: OnnxRuntimeGenAI::OnnxRuntimeGenAI

if(OnnxRuntimeGenAI_FOUND)
    return()
endif()

# ---------------------------------------------------------------------------
# Option 1: Local ORT GenAI build (ORT_GENAI_HOME)
# Expects:  <ORT_GENAI_HOME>/src/ort_genai.h  (headers)
#           <ORT_GENAI_HOME>/build/Windows/Debug/Debug/onnxruntime-genai.dll (Windows Debug)
# ---------------------------------------------------------------------------
if(ORT_GENAI_HOME)
    message(STATUS "Using local ORT GenAI from: ${ORT_GENAI_HOME}")

    set(_GENAI_HEADER_DIR "${ORT_GENAI_HOME}/src")
    if(NOT EXISTS "${_GENAI_HEADER_DIR}/ort_genai_c.h")
        message(FATAL_ERROR "ort_genai_c.h not found at ${_GENAI_HEADER_DIR}/ort_genai_c.h")
    endif()

    # Determine library path based on platform and config
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_GENAI_LIB_DIR "${ORT_GENAI_HOME}/build/Windows/${CMAKE_BUILD_TYPE}/${CMAKE_BUILD_TYPE}")
        set(_GENAI_DLL "onnxruntime-genai.dll")
        set(_GENAI_LIB "onnxruntime-genai.lib")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_GENAI_LIB_DIR "${ORT_GENAI_HOME}/build/Linux/${CMAKE_BUILD_TYPE}")
        set(_GENAI_DLL "libonnxruntime-genai.so")
        set(_GENAI_LIB "")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(_GENAI_LIB_DIR "${ORT_GENAI_HOME}/build/macOS/${CMAKE_BUILD_TYPE}")
        set(_GENAI_DLL "libonnxruntime-genai.dylib")
        set(_GENAI_LIB "")
    elseif(ANDROID)
        set(_GENAI_LIB_DIR "${ORT_GENAI_HOME}/build/Android/${CMAKE_BUILD_TYPE}")
        set(_GENAI_DLL "libonnxruntime-genai.so")
        set(_GENAI_LIB "")
    endif()

    add_library(OnnxRuntimeGenAI::OnnxRuntimeGenAI SHARED IMPORTED)
    set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_GENAI_HEADER_DIR}"
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
            IMPORTED_IMPLIB "${_GENAI_LIB_DIR}/${_GENAI_LIB}"
            IMPORTED_LOCATION "${_GENAI_LIB_DIR}/${_GENAI_DLL}"
        )
    else()
        set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
            IMPORTED_LOCATION "${_GENAI_LIB_DIR}/${_GENAI_DLL}"
            IMPORTED_NO_SONAME TRUE
        )
    endif()

    set(ORT_GENAI_HEADER_DIR "${_GENAI_HEADER_DIR}" CACHE PATH "OnnxRuntimeGenAI include directory" FORCE)
    set(ORT_GENAI_LIB_DIR "${_GENAI_LIB_DIR}" CACHE PATH "OnnxRuntimeGenAI native library directory" FORCE)

    set(OnnxRuntimeGenAI_FOUND TRUE)
    message(STATUS "ORT_GENAI_HEADER_DIR: ${_GENAI_HEADER_DIR}")
    message(STATUS "ORT_GENAI_LIB_DIR:    ${_GENAI_LIB_DIR}")
    return()
endif()

# ---------------------------------------------------------------------------
# Option 2: NuGet package (default)
# ---------------------------------------------------------------------------

include(FetchContent)

# Determine platform suffix for runtimes/ directory
if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_GENAI_PLATFORM "android-arm64")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(_GENAI_PLATFORM "android-x64")
    else()
        message(FATAL_ERROR "Unsupported Android ABI for OnnxRuntimeGenAI: ${ANDROID_ABI}")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_GENAI_PLATFORM "linux-arm64")
    else()
        set(_GENAI_PLATFORM "linux-x64")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_GENAI_PLATFORM "osx-arm64")
    else()
        set(_GENAI_PLATFORM "osx-x64")
    endif()
elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "arm64"
        OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64EC" OR CMAKE_GENERATOR_PLATFORM STREQUAL "arm64EC")
    set(_GENAI_PLATFORM "win-arm64")
elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "")
    set(_GENAI_PLATFORM "win-x64")
else()
    message(FATAL_ERROR "Unsupported platform for OnnxRuntimeGenAI: ${CMAKE_GENERATOR_PLATFORM} on ${CMAKE_SYSTEM_NAME}")
endif()

set(_GENAI_PACKAGE_NAME "Microsoft.ML.OnnxRuntimeGenAI.Foundry")
if(ANDROID)
    # The Foundry meta-package ships desktop RIDs only; the base package embeds
    # an AAR with the Android headers and native .so files. Mirrors the same
    # split handled in FindOnnxRuntime.cmake.
    set(_GENAI_PACKAGE_NAME "Microsoft.ML.OnnxRuntimeGenAI")
endif()

if(NOT ORT_GENAI_VERSION)
    # Single source of truth: sdk_v2/deps_versions.json. The Python SDK build
    # backend reads the same file. Override at the cmake command line with
    # -DORT_GENAI_VERSION=...
    set(_GENAI_DEPS_FILE "${CMAKE_CURRENT_LIST_DIR}/../../deps_versions.json")
    if(NOT EXISTS "${_GENAI_DEPS_FILE}")
        message(FATAL_ERROR "Required versions file not found: ${_GENAI_DEPS_FILE}")
    endif()
    file(READ "${_GENAI_DEPS_FILE}" _GENAI_DEPS_JSON)
    string(JSON ORT_GENAI_VERSION GET "${_GENAI_DEPS_JSON}" "onnxruntime-genai" "version")
    message(STATUS "ORT_GENAI_VERSION=${ORT_GENAI_VERSION} (from ${_GENAI_DEPS_FILE})")
endif()

# Allow the pipeline or caller to override the download URL (e.g., to use a local
# file:// path when direct nuget.org access is blocked in CI). When cross-compiling
# for Android the override must point at a package that ships the Android AAR --
# the base Microsoft.ML.OnnxRuntimeGenAI package, not the desktop-only .Foundry
# meta-package selected above.
if(NOT GENAI_FETCH_URL)
    # Dev builds come from the ADO nightly feed; release versions come from nuget.org.
    if(ORT_GENAI_VERSION MATCHES "-dev-")
        set(ORT_GENAI_FEED_ORG  "aiinfra")
        set(ORT_GENAI_FEED_PROJECT "2692857e-05ef-43b4-ba9c-ccf1c22c437c")
        set(ORT_GENAI_FEED_ID   "7982ae20-ed19-4a35-a362-a96ac99897b7")
        set(GENAI_FETCH_URL "https://pkgs.dev.azure.com/${ORT_GENAI_FEED_ORG}/${ORT_GENAI_FEED_PROJECT}/_apis/packaging/feeds/${ORT_GENAI_FEED_ID}/nuget/packages/${_GENAI_PACKAGE_NAME}/versions/${ORT_GENAI_VERSION}/content?api-version=6.0-preview.1")
        message(STATUS "Downloading ${_GENAI_PACKAGE_NAME} ${ORT_GENAI_VERSION} from ORT-Nightly feed")
    else()
        string(TOLOWER "${_GENAI_PACKAGE_NAME}" _GENAI_PACKAGE_LOWER)
        set(GENAI_FETCH_URL "https://api.nuget.org/v3-flatcontainer/${_GENAI_PACKAGE_LOWER}/${ORT_GENAI_VERSION}/${_GENAI_PACKAGE_LOWER}.${ORT_GENAI_VERSION}.nupkg")
        message(STATUS "Downloading ${_GENAI_PACKAGE_NAME} ${ORT_GENAI_VERSION} from nuget.org")
    endif()
else()
    message(STATUS "Using caller-provided GENAI_FETCH_URL: ${GENAI_FETCH_URL}")
endif()

# Normalize to forward slashes — backslashes from Windows paths cause CMake
# string-parsing errors inside FetchContent/ExternalProject_Add.
string(REPLACE "\\" "/" GENAI_FETCH_URL "${GENAI_FETCH_URL}")

# CMake's ExternalProject doesn't recognize .nupkg as an archive format.
# Since .nupkg is just a ZIP file, copy it with a .zip extension if it's a local file.
if(GENAI_FETCH_URL MATCHES "\\.nupkg$" AND NOT GENAI_FETCH_URL MATCHES "^https?://")
    set(_GENAI_ZIP_PATH "${CMAKE_BINARY_DIR}/_deps/genai-download/genai.zip")
    get_filename_component(_GENAI_ZIP_DIR "${_GENAI_ZIP_PATH}" DIRECTORY)
    file(MAKE_DIRECTORY "${_GENAI_ZIP_DIR}")
    file(COPY_FILE "${GENAI_FETCH_URL}" "${_GENAI_ZIP_PATH}")
    set(GENAI_FETCH_URL "${_GENAI_ZIP_PATH}")
    message(STATUS "Copied .nupkg to .zip for CMake extraction: ${GENAI_FETCH_URL}")
endif()

FetchContent_Declare(genailib
    URL ${GENAI_FETCH_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_NAME genai.zip  # .nupkg is a ZIP; force CMake to recognize the format
)
FetchContent_MakeAvailable(genailib)

if(ANDROID)
    # The NuGet package embeds an AAR with the headers and native .so files.
    # Extract it to get headers/ and jni/<abi>/libonnxruntime-genai.so.
    set(_GENAI_ANDROID_DIR "${genailib_SOURCE_DIR}/runtimes/android/native")
    set(_GENAI_AAR_PATH "${_GENAI_ANDROID_DIR}/onnxruntime-genai.aar")
    if(NOT EXISTS "${_GENAI_AAR_PATH}")
        # Most likely cause: the package that was fetched has no Android binaries.
        # Only the base Microsoft.ML.OnnxRuntimeGenAI package ships them; the
        # .Foundry meta-package is desktop-only. A caller-supplied
        # GENAI_FETCH_URL bypasses the package selection above, so it must point
        # at the base package when cross-compiling for Android.
        string(CONCAT _GENAI_AAR_HINT
               "Expected the base Microsoft.ML.OnnxRuntimeGenAI package, which ships Android binaries; "
               "the .Foundry meta-package does not.")
        if(GENAI_FETCH_URL)
            string(CONCAT _GENAI_AAR_HINT "${_GENAI_AAR_HINT}"
                   " GENAI_FETCH_URL was supplied by the caller (${GENAI_FETCH_URL});"
                   " point it at the base package for Android builds.")
        endif()
        message(FATAL_ERROR "GenAI Android AAR not found at ${_GENAI_AAR_PATH}. ${_GENAI_AAR_HINT}")
    endif()
    if(NOT EXISTS "${_GENAI_ANDROID_DIR}/jni")
        file(ARCHIVE_EXTRACT INPUT "${_GENAI_AAR_PATH}" DESTINATION "${_GENAI_ANDROID_DIR}/")
    endif()
    set(_GENAI_HEADER_DIR "${_GENAI_ANDROID_DIR}/headers")
    set(_GENAI_LIB_DIR    "${_GENAI_ANDROID_DIR}/jni/${ANDROID_ABI}")
    message(STATUS "Extracted GenAI Android AAR: ${_GENAI_AAR_PATH}")
else()
    set(_GENAI_HEADER_DIR "${genailib_SOURCE_DIR}/build/native/include")
    set(_GENAI_LIB_DIR    "${genailib_SOURCE_DIR}/runtimes/${_GENAI_PLATFORM}/native")
endif()

# Validate
if(NOT EXISTS "${_GENAI_HEADER_DIR}/ort_genai_c.h")
    message(FATAL_ERROR "OnnxRuntimeGenAI header not found at ${_GENAI_HEADER_DIR}/ort_genai_c.h")
endif()

# Create imported target
add_library(OnnxRuntimeGenAI::OnnxRuntimeGenAI SHARED IMPORTED)
set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_GENAI_HEADER_DIR}"
)

if(ANDROID)
    if(NOT EXISTS "${_GENAI_LIB_DIR}/libonnxruntime-genai.so")
        message(FATAL_ERROR "libonnxruntime-genai.so not found at ${_GENAI_LIB_DIR}/libonnxruntime-genai.so")
    endif()
    set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
        IMPORTED_LOCATION "${_GENAI_LIB_DIR}/libonnxruntime-genai.so"
        IMPORTED_NO_SONAME TRUE
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT EXISTS "${_GENAI_LIB_DIR}/libonnxruntime-genai.so")
        message(FATAL_ERROR "libonnxruntime-genai.so not found at ${_GENAI_LIB_DIR}/libonnxruntime-genai.so")
    endif()
    set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
        IMPORTED_LOCATION "${_GENAI_LIB_DIR}/libonnxruntime-genai.so"
        IMPORTED_NO_SONAME TRUE
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT EXISTS "${_GENAI_LIB_DIR}/libonnxruntime-genai.dylib")
        message(FATAL_ERROR "libonnxruntime-genai.dylib not found at ${_GENAI_LIB_DIR}/libonnxruntime-genai.dylib")
    endif()
    set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
        IMPORTED_LOCATION "${_GENAI_LIB_DIR}/libonnxruntime-genai.dylib"
        IMPORTED_NO_SONAME TRUE
    )
else()
    if(NOT EXISTS "${_GENAI_LIB_DIR}/onnxruntime-genai.lib")
        message(FATAL_ERROR "onnxruntime-genai.lib not found at ${_GENAI_LIB_DIR}/onnxruntime-genai.lib")
    endif()
    set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
        IMPORTED_IMPLIB "${_GENAI_LIB_DIR}/onnxruntime-genai.lib"
    )
    if(EXISTS "${_GENAI_LIB_DIR}/onnxruntime-genai.dll")
        set_target_properties(OnnxRuntimeGenAI::OnnxRuntimeGenAI PROPERTIES
            IMPORTED_LOCATION "${_GENAI_LIB_DIR}/onnxruntime-genai.dll"
        )
    endif()
endif()

# Export paths for downstream use
set(ORT_GENAI_HEADER_DIR "${_GENAI_HEADER_DIR}" CACHE PATH "OnnxRuntimeGenAI include directory" FORCE)
set(ORT_GENAI_LIB_DIR "${_GENAI_LIB_DIR}" CACHE PATH "OnnxRuntimeGenAI native library directory" FORCE)

set(OnnxRuntimeGenAI_FOUND TRUE)
message(STATUS "OnnxRuntimeGenAI package: ${_GENAI_PACKAGE_NAME} ${ORT_GENAI_VERSION}")
message(STATUS "ORT_GENAI_HEADER_DIR: ${_GENAI_HEADER_DIR}")
message(STATUS "ORT_GENAI_LIB_DIR:    ${_GENAI_LIB_DIR}")
