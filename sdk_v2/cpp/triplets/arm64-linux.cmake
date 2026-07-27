# Custom triplet that adds -fPIC so static vcpkg libraries can be linked
# into the foundry_local shared library (libfoundry_local.so).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")
