// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/request.h"

#include <functional>
#include <memory>

namespace fl::api {

// Associates cancellation with one admitted invocation, never with a reusable request pointer alone.
class RequestAdmission {
 public:
  using CancelAction = std::function<void(Request::CancellationReason, const std::string&)>;

  RequestAdmission(const Request& request, CancelAction cancel);
  ~RequestAdmission();
  RequestAdmission(const RequestAdmission&) = delete;
  RequestAdmission& operator=(const RequestAdmission&) = delete;
  RequestAdmission(RequestAdmission&&) = delete;
  RequestAdmission& operator=(RequestAdmission&&) = delete;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Unregistered requests retain their ordinary embedded cancellation semantics.
void CancelRequest(const Request& request, Request::CancellationReason reason = Request::CancellationReason::Caller,
                   std::string_view detail = {}) noexcept;

}  // namespace fl::api
