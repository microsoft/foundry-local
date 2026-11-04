// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/request_admission.h"

#include "exception.h"

#include <map>
#include <mutex>
#include <utility>

namespace fl::api {
namespace {

struct Invocation {
  RequestAdmission::CancelAction cancel;
  bool notified = false;
};

std::mutex admission_mutex;
std::map<const Request*, std::shared_ptr<Invocation>> invocations;

}  // namespace

struct RequestAdmission::State {
  const Request& request;
  std::shared_ptr<Invocation> invocation;
};

RequestAdmission::RequestAdmission(const Request& request, CancelAction cancel)
    : state_(std::make_unique<State>(request, std::make_shared<Invocation>(std::move(cancel)))) {
  std::lock_guard lock(admission_mutex);
  if (!request.TryBegin()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request is already being processed");
  }

  try {
    if (!invocations.emplace(&request, state_->invocation).second) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request already has an admitted invocation");
    }
  } catch (...) {
    request.PublishCompletion();
    throw;
  }
}

RequestAdmission::~RequestAdmission() {
  std::map<const Request*, std::shared_ptr<Invocation>>::node_type retired;
  {
    std::lock_guard lock(admission_mutex);
    const auto found = invocations.find(&state_->request);
    if (found != invocations.end() && found->second == state_->invocation) {
      retired = invocations.extract(found);
    }

    state_->request.PublishCompletion();
  }
}

void CancelRequest(const Request& request, Request::CancellationReason reason, std::string_view detail) noexcept {
  std::shared_ptr<Invocation> invocation;
  Request::CancellationReason actual_reason = reason;
  std::string actual_detail;
  try {
    {
      std::lock_guard lock(admission_mutex);
      const bool canceled = reason == Request::CancellationReason::StreamingCallbackException
                                ? request.CancelFromStreamingCallbackException(detail)
                                : request.Cancel(reason);
      const auto found = invocations.find(&request);
      if (canceled && found != invocations.end() && !found->second->notified) {
        invocation = found->second;
        invocation->notified = true;
        actual_reason = request.GetCancellationReason();
        try {
          actual_detail = request.CancellationDetail();
        } catch (...) {
        }
      }
    }

    if (invocation && invocation->cancel) {
      invocation->cancel(actual_reason, actual_detail);
    }
  } catch (...) {
    // Transport failure is reported by the processing call; cancellation itself stays noexcept.
  }
}

}  // namespace fl::api
