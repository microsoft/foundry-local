// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <windows.h>
#include <sddl.h>

#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fl::hosting::windows {

inline void Check(bool success, const char* operation) {
  if (!success) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
  }
}

struct LocalFreeDeleter {
  void operator()(void* value) const noexcept { LocalFree(value); }
};

using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

inline std::vector<std::byte> ProcessUser(HANDLE process) {
  HANDLE raw_token = nullptr;
  Check(OpenProcessToken(process, TOKEN_QUERY, &raw_token) != FALSE, "OpenProcessToken");
  const std::unique_ptr<void, decltype(&CloseHandle)> token(raw_token, &CloseHandle);
  DWORD size{};
  if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    Check(false, "GetTokenInformation(size)");
  }

  std::vector<std::byte> user(size);
  Check(GetTokenInformation(token.get(), TokenUser, user.data(), size, &size) != FALSE, "GetTokenInformation");
  return user;
}

inline PSID UserSid(const std::vector<std::byte>& user) {
  return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
}

inline std::wstring UserSidText() {
  const auto user = ProcessUser(GetCurrentProcess());
  LPWSTR raw_sid = nullptr;
  Check(ConvertSidToStringSidW(UserSid(user), &raw_sid) != FALSE, "ConvertSidToStringSid");
  const LocalMemory sid(raw_sid);
  return raw_sid;
}

inline LocalMemory OwnerSecurity(bool directory = false) {
  const auto sid = UserSidText();
  const auto sddl = L"O:" + sid + (directory ? L"D:P(A;OICI;FA;;;" : L"D:P(A;;GA;;;") + sid + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) !=
            FALSE,
        "ConvertStringSecurityDescriptorToSecurityDescriptor");
  return LocalMemory(descriptor);
}

}  // namespace fl::hosting::windows
