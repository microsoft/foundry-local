// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry_redaction.h"

#include <EventProperties.hpp>
#include <EventProperty.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fl::TelemetryInternal {

namespace detail {

inline bool IsSecretProperty(std::string_view name) {
  std::string normalized(name);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

  constexpr std::string_view secret_names[] = {
      "access-key",
      "access_key",
      "accesskey",
      "access-token",
      "access_token",
      "account-key",
      "account_key",
      "accountkey",
      "api-key",
      "api_key",
      "apikey",
      "auth",
      "authorization",
      "client-secret",
      "client_secret",
      "connection-string",
      "connection_string",
      "connectionstring",
      "credential",
      "credentials",
      "password",
      "passwd",
      "private-key",
      "private_key",
      "privatekey",
      "pwd",
      "secret",
      "sig",
      "signature",
      "token",
  };
  for (const auto secret_name : secret_names) {
    if (normalized == secret_name ||
        (normalized.size() > secret_name.size() && normalized.ends_with(secret_name))) {
      return true;
    }
  }
  return false;
}

inline size_t FindUrlAnchor(std::string_view value) {
  size_t separator = value.find("://");
  while (separator != std::string_view::npos) {
    size_t start = separator;
    while (start > 0) {
      const unsigned char character = static_cast<unsigned char>(value[start - 1]);
      if (!std::isalnum(character) && value[start - 1] != '+' && value[start - 1] != '-' &&
          value[start - 1] != '.') {
        break;
      }
      --start;
    }
    if (start < separator && std::isalpha(static_cast<unsigned char>(value[start]))) {
      return start;
    }
    separator = value.find("://", separator + 3);
  }
  return std::string_view::npos;
}

inline size_t FindAbsolutePathAnchor(std::string_view value) {
  for (size_t i = 0; i + 1 < value.size(); ++i) {
    if (value[i] != '/' || value[i + 1] == '/' ||
        std::isspace(static_cast<unsigned char>(value[i + 1]))) {
      continue;
    }

    if (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1])) ||
        value[i - 1] == ':' || value[i - 1] == '=' || value[i - 1] == '"' ||
        value[i - 1] == '\'' || value[i - 1] == '(') {
      return i;
    }
  }
  return std::string_view::npos;
}

inline std::string SanitizeString(std::string_view value) {
  const size_t url_anchor = FindUrlAnchor(value);
  std::string without_url(value.substr(0, url_anchor));
  if (url_anchor != std::string_view::npos) {
    without_url += "[url]";
  }

  const size_t path_anchor = FindAbsolutePathAnchor(without_url);
  if (path_anchor != std::string_view::npos) {
    without_url.replace(path_anchor, std::string::npos, "[path]");
  }
  return ScrubStringForTelemetry(without_url);
}

inline std::string SanitizeMetadataValue(std::string_view value) {
  for (size_t separator = value.find_first_of(":="); separator != std::string_view::npos;
       separator = value.find_first_of(":=", separator + 1)) {
    size_t end = separator;
    while (end > 0 && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
      --end;
    }
    size_t start = end;
    while (start > 0) {
      const unsigned char character = static_cast<unsigned char>(value[start - 1]);
      if (!std::isalnum(character) && character != '_' && character != '-' && character != '.') {
        break;
      }
      --start;
    }

    if (start < end && IsSecretProperty(value.substr(start, end - start))) {
      return SanitizeString(std::string(value.substr(0, separator + 1)) + "[secret]");
    }
  }

  return SanitizeString(value);
}

inline void SanitizeProperties(::Microsoft::Applications::Events::EventProperties& event_properties,
                               ::Microsoft::Applications::Events::DataCategory category) {
  using ::Microsoft::Applications::Events::EventProperty;

  // 1DS exposes its category maps as const and its setter always targets PartC. The event is mutable here, so update
  // mapped values in place to preserve PartB/PartC placement and stable schema keys.
  auto& properties =
      const_cast<std::map<std::string, EventProperty>&>(event_properties.GetProperties(category));
  for (auto& [name, property] : properties) {
    const bool secret_property = IsSecretProperty(name);
    if (property.type == EventProperty::TYPE_STRING) {
      const auto value = property.as_string == nullptr ? std::string_view{} : std::string_view(property.as_string);
      const auto sanitized_value = secret_property ? std::string{"[secret]"} : SanitizeMetadataValue(value);
      property = EventProperty(sanitized_value, property.piiKind, property.dataCategory);
    } else if (property.type == EventProperty::TYPE_STRING_ARRAY) {
      std::vector<std::string> sanitized_values;
      if (property.as_stringArray != nullptr) {
        sanitized_values.reserve(property.as_stringArray->size());
        for (const auto& value : *property.as_stringArray) {
          sanitized_values.push_back(secret_property ? std::string{"[secret]"} : SanitizeMetadataValue(value));
        }
      }

      property = EventProperty(sanitized_values, property.piiKind, property.dataCategory);
    }
  }
}

}  // namespace detail

inline std::string SanitizeCommonContextValue(std::string_view value) {
  return detail::SanitizeMetadataValue(value);
}

// Enforces telemetry string privacy and size limits at the final EventProperties emission boundary.
inline void SanitizeEventProperties(::Microsoft::Applications::Events::EventProperties& event_properties) {
  detail::SanitizeProperties(event_properties, ::Microsoft::Applications::Events::DataCategory_PartC);
  detail::SanitizeProperties(event_properties, ::Microsoft::Applications::Events::DataCategory_PartB);
}

}  // namespace fl::TelemetryInternal
