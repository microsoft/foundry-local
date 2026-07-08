# Copyright (c) Microsoft. All rights reserved.
# Find/acquire ONNX Runtime.
#
# Sources ORT from Microsoft.ML.OnnxRuntime.Foundry (or Microsoft.ML.OnnxRuntime
# on Android) via FetchContent — nuget.org for releases, the ORT-Nightly ADO
# feed for -dev- versions. The version comes from sdk_v2/deps_versions.json and
# is shared by all platforms.
#
# Creates an IMPORTED target: OnnxRuntime::OnnxRuntime

if(OnnxRuntime_FOUND)
    return()
endif()

include(FetchContent)

# Determine platform suffix for runtimes/ directory
if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_ORT_PLATFORM "android-arm64")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(_ORT_PLATFORM "android-x64")
    else()
        message(FATAL_ERROR "Unsupported Android ABI for OnnxRuntime: ${ANDROID_ABI}")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ORT_PLATFORM "linux-x64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_ORT_PLATFORM "osx-arm64")
    else()
        set(_ORT_PLATFORM "osx-x64")
    endif()
elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "arm64"
        OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64EC" OR CMAKE_GENERATOR_PLATFORM STREQUAL "arm64EC")
    set(_ORT_PLATFORM "win-arm64")
elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "")
    set(_ORT_PLATFORM "win-x64")
else()
    message(FATAL_ERROR "Unsupported platform for OnnxRuntime: ${CMAKE_GENERATOR_PLATFORM} on ${CMAKE_SYSTEM_NAME}")
endif()

if(ORT_HOME)
    # Use a pre-extracted ORT directory (e.g. from build.py --ort_home).
    # Android: expects headers/ and jni/<abi>/ from an extracted AAR.
    # Other platforms: expects include/ and lib/.
    get_filename_component(ORT_HOME "${ORT_HOME}" ABSOLUTE)
    message(STATUS "Using OnnxRuntime from ORT_HOME: ${ORT_HOME}")

    if(ANDROID)
        set(_ORT_HEADER_DIR "${ORT_HOME}/headers")
        set(_ORT_LIB_DIR "${ORT_HOME}/jni/${ANDROID_ABI}")
    else()
        set(_ORT_HEADER_DIR "${ORT_HOME}/include")
        set(_ORT_LIB_DIR "${ORT_HOME}/lib")
    endif()
else()
    # -----------------------------------------------------------------------
    # Standard path: FetchContent from nuget.org (releases) or ORT-Nightly ADO feed (dev builds)
    # -----------------------------------------------------------------------
    if(NOT ORT_VERSION)
        # Single source of truth: sdk_v2/deps_versions.json. The Python SDK
        # build backend reads the same file so wheel deps and native ABI
        # always agree. Override at the cmake command line with -DORT_VERSION=...
        set(_DEPS_FILE "${CMAKE_CURRENT_LIST_DIR}/../../deps_versions.json")
        if(NOT EXISTS "${_DEPS_FILE}")
            message(FATAL_ERROR "Required versions file not found: ${_DEPS_FILE}")
        endif()
        file(READ "${_DEPS_FILE}" _DEPS_JSON)
        string(JSON ORT_VERSION GET "${_DEPS_JSON}" "onnxruntime" "version")
        message(STATUS "ORT_VERSION=${ORT_VERSION} (from ${_DEPS_FILE})")
    endif()
    if(NOT ORT_PACKAGE_NAME)
        if(ANDROID)
            # The Foundry meta-package may not contain Android binaries;
            # use the base ORT package which includes the AAR.
            set(ORT_PACKAGE_NAME "Microsoft.ML.OnnxRuntime")
        else()
            set(ORT_PACKAGE_NAME "Microsoft.ML.OnnxRuntime.Foundry")
        endif()
    endif()

    # ORT_FETCH_URL can be set externally (e.g. for CI where nuget.org is blocked).
    set(ORT_FETCH_URL "" CACHE STRING "Override URL or local path for the OnnxRuntime NuGet package")

    if(NOT ORT_FETCH_URL)
        # Dev builds come from the ADO nightly feed; release versions come from nuget.org.
        if(ORT_VERSION MATCHES "-dev-")
            set(ORT_FEED_ORG  "aiinfra")
            set(ORT_FEED_PROJECT "2692857e-05ef-43b4-ba9c-ccf1c22c437c")
            set(ORT_FEED_ID   "7982ae20-ed19-4a35-a362-a96ac99897b7")
            set(ORT_FETCH_URL "https://pkgs.dev.azure.com/${ORT_FEED_ORG}/${ORT_FEED_PROJECT}/_apis/packaging/feeds/${ORT_FEED_ID}/nuget/packages/${ORT_PACKAGE_NAME}/versions/${ORT_VERSION}/content?api-version=6.0-preview.1")
            message(STATUS "Downloading ${ORT_PACKAGE_NAME} ${ORT_VERSION} from ORT-Nightly feed")
        else()
            string(TOLOWER "${ORT_PACKAGE_NAME}" _ORT_PACKAGE_LOWER)
            set(ORT_FETCH_URL "https://api.nuget.org/v3-flatcontainer/${_ORT_PACKAGE_LOWER}/${ORT_VERSION}/${_ORT_PACKAGE_LOWER}.${ORT_VERSION}.nupkg")
            message(STATUS "Downloading ${ORT_PACKAGE_NAME} ${ORT_VERSION} from nuget.org")
        endif()
    else()
        message(STATUS "Using pre-configured ORT_FETCH_URL: ${ORT_FETCH_URL}")
    endif()

    # Normalize backslashes (Windows paths) and handle .nupkg extension
    string(REPLACE "\\" "/" ORT_FETCH_URL "${ORT_FETCH_URL}")
    if(ORT_FETCH_URL MATCHES "\\.nupkg$" AND NOT ORT_FETCH_URL MATCHES "^https?://")
        set(_ORT_ZIP_PATH "${CMAKE_BINARY_DIR}/_deps/ortlib-download/ort.zip")
        get_filename_component(_ORT_ZIP_DIR "${_ORT_ZIP_PATH}" DIRECTORY)
        file(MAKE_DIRECTORY "${_ORT_ZIP_DIR}")
        file(COPY_FILE "${ORT_FETCH_URL}" "${_ORT_ZIP_PATH}")
        set(ORT_FETCH_URL "${_ORT_ZIP_PATH}")
    endif()

    FetchContent_Declare(ortlib URL ${ORT_FETCH_URL} DOWNLOAD_EXTRACT_TIMESTAMP TRUE DOWNLOAD_NAME ort.zip)
    FetchContent_MakeAvailable(ortlib)

    set(_ORT_HEADER_DIR "${ortlib_SOURCE_DIR}/build/native/include")
    set(_ORT_LIB_DIR    "${ortlib_SOURCE_DIR}/runtimes/${_ORT_PLATFORM}/native")

    message(STATUS "OnnxRuntime via FetchContent: ${ortlib_SOURCE_DIR}")

    if(ANDROID)
        # The NuGet package embeds an AAR with native .so files.
        # Extract it to get jni/<abi>/libonnxruntime.so.
        set(_ORT_AAR_PATH "${ortlib_SOURCE_DIR}/runtimes/android/native/onnxruntime.aar")
        if(NOT EXISTS "${_ORT_AAR_PATH}")
            message(FATAL_ERROR "ORT Android AAR not found at ${_ORT_AAR_PATH}. "
                "Ensure the ORT_PACKAGE_NAME contains Android binaries (e.g. Microsoft.ML.OnnxRuntime).")
        endif()

        file(ARCHIVE_EXTRACT INPUT "${_ORT_AAR_PATH}"
             DESTINATION "${ortlib_SOURCE_DIR}/runtimes/android/native/")
        set(_ORT_LIB_DIR "${ortlib_SOURCE_DIR}/runtimes/android/native/jni/${ANDROID_ABI}")
        message(STATUS "Extracted ORT Android AAR: ${_ORT_AAR_PATH}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # On Linux the Foundry meta-package doesn't contain libonnxruntime.so directly —
        # it's in the Microsoft.ML.OnnxRuntime.Gpu.Linux dependency package.
        set(_ORT_GPU_LINUX_PACKAGE "Microsoft.ML.OnnxRuntime.Gpu.Linux")

        # ORT_GPU_LINUX_FETCH_URL can be set externally (e.g. for CI where nuget.org is blocked).
        set(ORT_GPU_LINUX_FETCH_URL "" CACHE STRING "Override URL or local path for the ORT GPU Linux NuGet package")

        if(NOT ORT_GPU_LINUX_FETCH_URL)
            if(ORT_VERSION MATCHES "-dev-")
                set(ORT_GPU_LINUX_FETCH_URL "https://pkgs.dev.azure.com/${ORT_FEED_ORG}/${ORT_FEED_PROJECT}/_apis/packaging/feeds/${ORT_FEED_ID}/nuget/packages/${_ORT_GPU_LINUX_PACKAGE}/versions/${ORT_VERSION}/content?api-version=6.0-preview.1")
                message(STATUS "Downloading ${_ORT_GPU_LINUX_PACKAGE} ${ORT_VERSION} from ORT-Nightly feed")
            else()
                string(TOLOWER "${_ORT_GPU_LINUX_PACKAGE}" _ORT_GPU_LINUX_LOWER)
                set(ORT_GPU_LINUX_FETCH_URL "https://api.nuget.org/v3-flatcontainer/${_ORT_GPU_LINUX_LOWER}/${ORT_VERSION}/${_ORT_GPU_LINUX_LOWER}.${ORT_VERSION}.nupkg")
                message(STATUS "Downloading ${_ORT_GPU_LINUX_PACKAGE} ${ORT_VERSION} from nuget.org")
            endif()
        else()
            message(STATUS "Using pre-configured ORT_GPU_LINUX_FETCH_URL: ${ORT_GPU_LINUX_FETCH_URL}")
        endif()

        # Normalize backslashes and handle .nupkg extension
        string(REPLACE "\\" "/" ORT_GPU_LINUX_FETCH_URL "${ORT_GPU_LINUX_FETCH_URL}")
        if(ORT_GPU_LINUX_FETCH_URL MATCHES "\\.nupkg$" AND NOT ORT_GPU_LINUX_FETCH_URL MATCHES "^https?://")
            set(_ORT_GPU_ZIP_PATH "${CMAKE_BINARY_DIR}/_deps/ort_gpu_linux-download/ort_gpu_linux.zip")
            get_filename_component(_ORT_GPU_ZIP_DIR "${_ORT_GPU_ZIP_PATH}" DIRECTORY)
            file(MAKE_DIRECTORY "${_ORT_GPU_ZIP_DIR}")
            file(COPY_FILE "${ORT_GPU_LINUX_FETCH_URL}" "${_ORT_GPU_ZIP_PATH}")
            set(ORT_GPU_LINUX_FETCH_URL "${_ORT_GPU_ZIP_PATH}")
        endif()

        FetchContent_Declare(ort_gpu_linux URL ${ORT_GPU_LINUX_FETCH_URL} DOWNLOAD_EXTRACT_TIMESTAMP TRUE DOWNLOAD_NAME ort_gpu_linux.zip)
        FetchContent_MakeAvailable(ort_gpu_linux)

        set(_ORT_LIB_DIR "${ort_gpu_linux_SOURCE_DIR}/runtimes/${_ORT_PLATFORM}/native")
        message(STATUS "OnnxRuntime GPU Linux package: ${ort_gpu_linux_SOURCE_DIR}")
    endif()
endif()

# Validate that the expected files exist
if(NOT EXISTS "${_ORT_HEADER_DIR}/onnxruntime_c_api.h")
    message(FATAL_ERROR "OnnxRuntime header not found at ${_ORT_HEADER_DIR}/onnxruntime_c_api.h")
endif()

# Create imported target
add_library(OnnxRuntime::OnnxRuntime SHARED IMPORTED)
set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_ORT_HEADER_DIR}"
)

if(ANDROID)
    if(NOT EXISTS "${_ORT_LIB_DIR}/libonnxruntime.so")
        message(FATAL_ERROR "libonnxruntime.so not found at ${_ORT_LIB_DIR}/libonnxruntime.so")
    endif()
    set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
        IMPORTED_LOCATION "${_ORT_LIB_DIR}/libonnxruntime.so"
        IMPORTED_NO_SONAME TRUE
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT EXISTS "${_ORT_LIB_DIR}/libonnxruntime.so")
        message(FATAL_ERROR "libonnxruntime.so not found at ${_ORT_LIB_DIR}/libonnxruntime.so")
    endif()

    # The nuget package ships libonnxruntime.so with SONAME "libonnxruntime.so.1" baked in.
    # Create the versioned symlink so downstream executables can resolve the SONAME at link time.
    if(NOT EXISTS "${_ORT_LIB_DIR}/libonnxruntime.so.1")
        file(CREATE_LINK "${_ORT_LIB_DIR}/libonnxruntime.so"
             "${_ORT_LIB_DIR}/libonnxruntime.so.1" SYMBOLIC)
    endif()

    set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
        IMPORTED_LOCATION "${_ORT_LIB_DIR}/libonnxruntime.so"
        IMPORTED_NO_SONAME TRUE
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT EXISTS "${_ORT_LIB_DIR}/libonnxruntime.dylib")
        message(FATAL_ERROR "libonnxruntime.dylib not found at ${_ORT_LIB_DIR}/libonnxruntime.dylib")
    endif()
    set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
        IMPORTED_LOCATION "${_ORT_LIB_DIR}/libonnxruntime.dylib"
        IMPORTED_NO_SONAME TRUE
    )
else()
    if(NOT EXISTS "${_ORT_LIB_DIR}/onnxruntime.lib")
        message(FATAL_ERROR "onnxruntime.lib not found at ${_ORT_LIB_DIR}/onnxruntime.lib")
    endif()
    set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
        IMPORTED_IMPLIB "${_ORT_LIB_DIR}/onnxruntime.lib"
    )
    # On Windows, the runtime DLL sits next to the import lib.
    if(NOT _ORT_DLL_DIR)
        set(_ORT_DLL_DIR "${_ORT_LIB_DIR}")
    endif()
    if(EXISTS "${_ORT_DLL_DIR}/onnxruntime.dll")
        set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
            IMPORTED_LOCATION "${_ORT_DLL_DIR}/onnxruntime.dll"
        )
    endif()
endif()

# Export paths for downstream use (e.g. copying DLLs to output)
set(ORT_HEADER_DIR "${_ORT_HEADER_DIR}" CACHE PATH "OnnxRuntime include directory" FORCE)
set(ORT_LIB_DIR "${_ORT_LIB_DIR}" CACHE PATH "OnnxRuntime native library directory" FORCE)
if(NOT _ORT_DLL_DIR)
    set(_ORT_DLL_DIR "${_ORT_LIB_DIR}")
endif()
set(ORT_DLL_DIR "${_ORT_DLL_DIR}" CACHE PATH "OnnxRuntime runtime DLL directory (Windows)" FORCE)

set(OnnxRuntime_FOUND TRUE)
message(STATUS "ORT_HEADER_DIR: ${_ORT_HEADER_DIR}")
message(STATUS "ORT_LIB_DIR:    ${_ORT_LIB_DIR}")
message(STATUS "ORT_DLL_DIR:    ${_ORT_DLL_DIR}")
