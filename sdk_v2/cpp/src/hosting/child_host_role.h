// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/process_watch.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace fl::hosting {

// Private bootstrap descriptor, carried through the existing versioned configuration API by the packaged executable.
inline constexpr char kChildHostOption[] = "FoundryLocal.Internal.ChildHost";
inline constexpr auto kFirstOwnerWindow = std::chrono::seconds{30};

struct ChildHostRole {
  std::filesystem::path endpoint;
  std::string profile_key;
  std::string incarnation;
  ProcessIdentity starter;
};

std::string EncodeChildHostRole(const ChildHostRole& role);
ChildHostRole DecodeChildHostRole(std::string_view value);

}  // namespace fl::hosting
