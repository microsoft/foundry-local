// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/process_watch.h"
#include "hosting/host_state.h"

#include <filesystem>
#include <optional>
#include <system_error>

namespace fl::hosting {

// Only this exception proves that the OS did not create a child. Other errors preserve pending launch state.
class HostLaunchRejected : public std::system_error {
 public:
  using std::system_error::system_error;
};

std::optional<ProcessIdentity> SpawnNativeHost(const std::filesystem::path& executable,
                                               const std::filesystem::path& bootstrap,
                                               const HostStartupLock& startup_lock, ProcessWatch::TimePoint deadline);

}  // namespace fl::hosting
