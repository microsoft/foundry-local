// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <foundry_local/foundry_local_c.h>

#include "util/code_location.h"
#include "util/stacktrace.h"
#include "util/string_utils.h"

#include <stdexcept>
#include <string>

namespace fl {

#if (_MSC_VER && !defined(__PRETTY_FUNCTION__))
#define __PRETTY_FUNCTION__ __FUNCTION__
#endif

#define FL_WHERE ::fl::CodeLocation(__FILE__, __LINE__, static_cast<const char*>(__FUNCTION__))

#define FL_WHERE_WITH_STACK \
  ::fl::CodeLocation(__FILE__, __LINE__, static_cast<const char*>(__PRETTY_FUNCTION__), ::fl::GetStackTrace())

#define FL_THROW(code, ...) \
  throw ::fl::Exception(FL_WHERE_WITH_STACK, ::fl::MakeString(__VA_ARGS__), code)

/// Exception type for all Foundry Local errors.
class Exception : public std::runtime_error {
 public:
  Exception(const CodeLocation& location, const std::string& message,
            flErrorCode code = FOUNDRY_LOCAL_ERROR_INTERNAL);

  /// Returns the associated error code.
  flErrorCode code() const noexcept { return code_; }

 private:
  flErrorCode code_;
};

}  // namespace fl
