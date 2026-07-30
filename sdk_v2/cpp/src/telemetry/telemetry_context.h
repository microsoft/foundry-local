// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <array>
#include <string>
#include <utility>

namespace fl::TelemetryInternal {

inline constexpr std::array<const char*, 5> kSuppressedCommonContextFields{
    "AppInfo.Language",
    "AppInfo.Name",
    "UserInfo.Language",
    "UserInfo.TimeZone",
    "M365aInfo.EnrolledTenantId",
};

template <typename Suppression>
bool TrySuppressContext(Suppression&& suppression) noexcept {
  try {
    std::forward<Suppression>(suppression)();
    return true;
  } catch (...) {
    return false;
  }
}

template <typename SemanticContext>
void SuppressUnneededCommonContext(SemanticContext& context) {
  for (const char* field : kSuppressedCommonContextFields) {
    context.SetCommonField(field, std::string{});
  }
}

}  // namespace fl::TelemetryInternal
