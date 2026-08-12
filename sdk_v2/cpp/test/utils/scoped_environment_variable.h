// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace fl::test {

inline std::optional<std::string> GetEnvironmentVariable(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  size_t length = 0;
  const auto error = _dupenv_s(&value, &length, name);
  const std::unique_ptr<char, decltype(&std::free)> buffer(value, &std::free);
  if (error != 0) {
    throw std::system_error(error, std::generic_category(), "_dupenv_s failed");
  }

  if (buffer == nullptr) {
    return std::nullopt;
  }

  return std::string(buffer.get());
#else
  const auto* value = std::getenv(name);
  return value == nullptr ? std::nullopt : std::make_optional<std::string>(value);
#endif
}

inline void SetEnvironmentVariable(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
  const auto error = _putenv_s(name, value.value_or("").c_str());
  if (error != 0) {
    throw std::system_error(error, std::generic_category(), "_putenv_s failed");
  }
#else
  const auto error = value.has_value() ? setenv(name, value->c_str(), 1) : unsetenv(name);
  if (error != 0) {
    throw std::system_error(errno, std::generic_category(), "failed to update environment");
  }
#endif
}

class ScopedEnvironmentVariable {
 public:
  ScopedEnvironmentVariable(const char* name, std::string value)
      : name_(name), previous_(GetEnvironmentVariable(name)) {
    SetEnvironmentVariable(name_, std::move(value));
  }

  ~ScopedEnvironmentVariable() {
    try {
      SetEnvironmentVariable(name_, previous_);
    } catch (...) {
    }
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

 private:
  const char* name_;
  std::optional<std::string> previous_;
};

}  // namespace fl::test
