// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry_redaction.h"

#include <array>
#include <string>
#include <string_view>

namespace fl::TelemetryInternal {

template <typename Logger>
void SetCommonEventIdentity(Logger& logger, std::string_view app_name, std::string_view hashed_device_id) {
  logger.SetContext("AppName", SanitizeTelemetryValue(app_name));
  logger.SetContext("deviceId", hashed_device_id.empty() ? std::string{"Unavailable"}
                                                         : SanitizeTelemetryValue(hashed_device_id));
}

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
