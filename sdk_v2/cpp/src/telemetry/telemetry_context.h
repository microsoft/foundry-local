// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <array>
#include <string>

namespace fl::TelemetryInternal {

inline constexpr std::array<const char*, 4> kSuppressedCommonContextFields{
    "AppInfo.Language",
    "UserInfo.Language",
    "UserInfo.TimeZone",
    "M365aInfo.EnrolledTenantId",
};

template <typename SemanticContext>
void SuppressUnneededCommonContext(SemanticContext& context) {
  for (const char* field : kSuppressedCommonContextFields) {
    context.SetCommonField(field, std::string{});
  }
}

template <typename SemanticContext>
void SetApplicationNameFromProcessName(SemanticContext& context, const std::string& process_name) {
  if (!process_name.empty() && process_name != "unknown") {
    context.SetCommonField("AppInfo.Name", process_name);
  }
}

}  // namespace fl::TelemetryInternal
