// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry_redaction.h"

#include <EventProperties.hpp>
#include <EventProperty.hpp>

#include <map>
#include <type_traits>
#include <vector>

namespace fl::TelemetryInternal {

using ::Microsoft::Applications::Events::DataCategory;
using ::Microsoft::Applications::Events::DataCategory_PartC;
using ::Microsoft::Applications::Events::PiiKind;
using ::Microsoft::Applications::Events::PiiKind_None;

inline constexpr size_t kMaxTelemetryStringArrayElements = 64;

namespace detail {

inline bool IsIdentifierProperty(std::string_view name) {
  return name == "ModelId" || name == "CorrelationId" || name == "ExecutionProvider" ||
         name == "ProviderName" || name == "AppSessionGuid";
}

inline std::string SanitizePropertyValue(std::string_view name, std::string_view value) {
  if (IsSecretProperty(name)) {
    return "[secret]";
  }
  if (name == "Format") {
    return SanitizeTelemetryCatalogFormat(value);
  }

  return IsIdentifierProperty(name) ? SanitizeTelemetryIdentifier(value) : SanitizeTelemetryValue(value);
}

inline std::vector<std::string> SanitizeStringArray(std::string_view name, const std::vector<std::string>& values) {
  const auto count = (std::min)(values.size(), kMaxTelemetryStringArrayElements);
  std::vector<std::string> sanitized_values;
  sanitized_values.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    sanitized_values.push_back(SanitizePropertyValue(name, values[i]));
  }
  if (values.size() > count) {
    sanitized_values.back() = "[oversized array]";
  }
  return sanitized_values;
}

inline void SanitizeProperties(::Microsoft::Applications::Events::EventProperties& event_properties,
                               ::Microsoft::Applications::Events::DataCategory category) {
  using ::Microsoft::Applications::Events::EventProperty;

  // 1DS exposes its category maps as const and its setter always targets PartC. The event is mutable here, so update
  // mapped values in place to preserve PartB/PartC placement and stable schema keys.
  auto& properties =
      const_cast<std::map<std::string, EventProperty>&>(event_properties.GetProperties(category));
  for (auto& [name, property] : properties) {
    if (property.type == EventProperty::TYPE_STRING) {
      const auto sanitized_value = SanitizePropertyValue(name, BoundedTelemetryCString(property.as_string));
      property = EventProperty(sanitized_value, property.piiKind, property.dataCategory);
    } else if (property.type == EventProperty::TYPE_STRING_ARRAY) {
      std::vector<std::string> sanitized_values;
      if (property.as_stringArray != nullptr) {
        sanitized_values = SanitizeStringArray(name, *property.as_stringArray);
      }

      property = EventProperty(sanitized_values, property.piiKind, property.dataCategory);
    }
  }
}

}  // namespace detail

// Bound strings before 1DS takes an owning copy, not only when the event is ready to upload.
class BoundedEventProperties : public ::Microsoft::Applications::Events::EventProperties {
 public:
  using EventProperties::EventProperties;

  void SetProperty(const char* name, std::string_view value, PiiKind pii_kind = PiiKind_None,
                   DataCategory category = DataCategory_PartC) {
    EventProperties::SetProperty(name, detail::SanitizePropertyValue(name, value), pii_kind, category);
  }

  void SetProperty(const char* name, const char* value, PiiKind pii_kind = PiiKind_None,
                   DataCategory category = DataCategory_PartC) {
    SetProperty(name, BoundedTelemetryCString(value), pii_kind, category);
  }

  void SetProperty(const char* name, const std::vector<std::string>& values, PiiKind pii_kind = PiiKind_None,
                   DataCategory category = DataCategory_PartC) {
    auto sanitized_values = detail::SanitizeStringArray(name, values);
    EventProperties::SetProperty(name, sanitized_values, pii_kind, category);
  }

  template <typename Value>
    requires std::is_arithmetic_v<Value>
  void SetProperty(const char* name, Value value, PiiKind pii_kind = PiiKind_None,
                   DataCategory category = DataCategory_PartC) {
    EventProperties::SetProperty(name, value, pii_kind, category);
  }
};

inline std::string SanitizeCommonContextValue(std::string_view value) {
  return SanitizeTelemetryValue(value);
}

inline void SanitizeEventProperties(::Microsoft::Applications::Events::EventProperties& event_properties) {
  detail::SanitizeProperties(event_properties, ::Microsoft::Applications::Events::DataCategory_PartC);
  detail::SanitizeProperties(event_properties, ::Microsoft::Applications::Events::DataCategory_PartB);
}

}  // namespace fl::TelemetryInternal
