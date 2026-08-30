# Copyright (c) Microsoft. All rights reserved.

# This example uses std::thread directly (see main.cc), which requires linking
# against the platform thread library. On glibc < 2.34 (e.g. manylinux_2_28),
# pthread symbols live in a separate libpthread.so and are not implicitly
# available, so we must link them explicitly via CMake's Threads package.
find_package(Threads REQUIRED)

add_executable(realtime_audio_example main.cc)
target_link_libraries(realtime_audio_example PRIVATE foundry_local_cpp Threads::Threads)
