# Custom triplet for Android arm64 (aarch64) devices.
# Static vcpkg libraries with -fPIC are bundled into the
# foundry_local shared library (libfoundry_local.so).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
# API 28 (Android 9.0 Pie) — NDK provides iconv from this level,
# avoiding autotools cross-compilation issues with libiconv.
set(VCPKG_CMAKE_SYSTEM_VERSION 28)

# Explicitly declare the host triplet so vcpkg knows this is a cross-compilation.
set(VCPKG_HOST_TRIPLET x64-windows)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DANDROID_ABI=arm64-v8a"
    "-DANDROID_PLATFORM=android-28"
)

# Chain-load the NDK toolchain so both vcpkg ports and the top-level project
# use the same NDK. build.py sets ANDROID_NDK_HOME when --android_ndk_path is given.
if(DEFINED ENV{ANDROID_NDK_HOME})
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
        "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
endif()
