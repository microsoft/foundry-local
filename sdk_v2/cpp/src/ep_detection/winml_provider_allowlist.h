// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace fl {

inline bool IsTrustedWinMLProvider(std::string_view provider_name) {
  constexpr std::array<std::string_view, 6> trusted_provider_names = {
      "MIGraphXExecutionProvider",
      "NvTensorRtRtxExecutionProvider",
      "OpenVINOExecutionProvider",
      "QNNExecutionProvider",
      "RyzenAILightExecutionProvider",
      "VitisAIExecutionProvider",
  };

  const auto equals_ignore_case = [](std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char lhs_char, char rhs_char) {
             return std::tolower(static_cast<unsigned char>(lhs_char)) ==
                    std::tolower(static_cast<unsigned char>(rhs_char));
           });
  };

  return std::any_of(trusted_provider_names.begin(), trusted_provider_names.end(),
                     [provider_name, equals_ignore_case](std::string_view trusted_name) {
                       return equals_ignore_case(provider_name, trusted_name);
                     });
}

}  // namespace fl
