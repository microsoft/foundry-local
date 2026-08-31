# Copyright (c) Microsoft. All rights reserved.

# std::thread requires an explicit pthread link on glibc < 2.34.
find_package(Threads REQUIRED)

add_executable(realtime_audio_example main.cc)
target_link_libraries(realtime_audio_example PRIVATE foundry_local_cpp Threads::Threads)
