cmake_minimum_required(VERSION 3.20)
project(foundry_local_cpp_samples LANGUAGES CXX)

set(FOUNDRY_LOCAL_SDK_VERSION "2.2.1")
set(FOUNDRY_LOCAL_PACKAGES_DIR "${CMAKE_CURRENT_BINARY_DIR}/packages/${FOUNDRY_LOCAL_SDK_VERSION}" CACHE PATH
    "Directory containing the restored Runtime NuGet package and its dependencies")

if(WIN32)
  set(_os "win")
elseif(APPLE)
  set(_os "osx")
else()
  set(_os "linux")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  set(_arch "arm64")
else()
  set(_arch "x64")
endif()
set(FOUNDRY_LOCAL_RID "${_os}-${_arch}" CACHE STRING "Runtime identifier for the target platform")

set(_sdk_package "${FOUNDRY_LOCAL_PACKAGES_DIR}/Microsoft.AI.Foundry.Local.Runtime.${FOUNDRY_LOCAL_SDK_VERSION}")
set(_sdk_native "${_sdk_package}/runtimes/${FOUNDRY_LOCAL_RID}/native")

file(GLOB _runtime_files
  "${FOUNDRY_LOCAL_PACKAGES_DIR}/*/runtimes/${FOUNDRY_LOCAL_RID}/native/*.dll"
  "${FOUNDRY_LOCAL_PACKAGES_DIR}/*/runtimes/${FOUNDRY_LOCAL_RID}/native/*.so*"
  "${FOUNDRY_LOCAL_PACKAGES_DIR}/*/runtimes/${FOUNDRY_LOCAL_RID}/native/*.dylib"
)
set(_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/runtime")
file(COPY ${_runtime_files} DESTINATION "${_runtime_dir}")
# ORT's NuGet filenames omit the SONAME suffix required by the native library.
if(APPLE)
  set(_ort_filename libonnxruntime.dylib)
  set(_ort_soname libonnxruntime.1.dylib)
elseif(UNIX)
  set(_ort_filename libonnxruntime.so)
  set(_ort_soname libonnxruntime.so.1)
endif()
if(UNIX)
  file(CREATE_LINK "${_ort_filename}" "${_runtime_dir}/${_ort_soname}" SYMBOLIC)
endif()

find_library(FOUNDRY_LOCAL_LIBRARY
  NAMES foundry_local
  PATHS "${_sdk_native}"
  NO_DEFAULT_PATH
  REQUIRED
)

add_library(foundry_local_sdk SHARED IMPORTED GLOBAL)
if(WIN32)
  set_target_properties(foundry_local_sdk PROPERTIES
    IMPORTED_IMPLIB "${FOUNDRY_LOCAL_LIBRARY}"
    IMPORTED_LOCATION "${_runtime_dir}/foundry_local.dll"
  )
else()
  get_filename_component(_library_name "${FOUNDRY_LOCAL_LIBRARY}" NAME)
  set_target_properties(foundry_local_sdk PROPERTIES IMPORTED_LOCATION "${_runtime_dir}/${_library_name}")
endif()
target_include_directories(foundry_local_sdk SYSTEM INTERFACE "${_sdk_package}/build/native/include")

function(add_foundry_sample target directory)
  add_executable(${target} "${directory}/main.cc")
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_link_libraries(${target} PRIVATE foundry_local_sdk)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  if(APPLE)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_runtime_files} "$<TARGET_FILE_DIR:${target}>"
    COMMAND_EXPAND_LISTS
  )
  if(UNIX)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink "${_ort_filename}" "$<TARGET_FILE_DIR:${target}>/${_ort_soname}"
    )
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

add_foundry_sample(hello-world hello-world)
add_foundry_sample(realtime-transcription realtime-transcription)
add_foundry_sample(embeddings embeddings)
add_foundry_sample(tool-calling tool-calling)
add_foundry_sample(web-server web-server)
add_foundry_sample(vision vision)
add_foundry_sample(byom byom)

find_package(Threads REQUIRED)
target_link_libraries(realtime-transcription PRIVATE Threads::Threads)

find_path(PORTAUDIO_INCLUDE_DIR portaudio.h)
find_library(PORTAUDIO_LIBRARY NAMES portaudio)
if(PORTAUDIO_INCLUDE_DIR AND PORTAUDIO_LIBRARY)
  target_compile_definitions(realtime-transcription PRIVATE FOUNDRY_SAMPLE_HAS_PORTAUDIO=1)
  target_include_directories(realtime-transcription PRIVATE "${PORTAUDIO_INCLUDE_DIR}")
  target_link_libraries(realtime-transcription PRIVATE "${PORTAUDIO_LIBRARY}")
endif()

if(WIN32)
  target_link_libraries(web-server PRIVATE ws2_32)
endif()
