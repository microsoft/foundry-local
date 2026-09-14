// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

// This header is private to the in-repo Node addon. It is not installed with the public C/C++ SDK.
#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#define FOUNDRY_LOCAL_NODE_PRIVATE_API_VERSION 1

struct flRequestPreflightOperation;

struct flNodePrivateApi {
  FL_API_STATUS(Session_CaptureRequestPreflight, _In_ const flSession* session, _In_ const flRequest* request,
                _Outptr_ flRequestPreflightOperation** out_operation);
  FL_API_STATUS(RequestPreflightOperation_Execute, _Inout_ flRequestPreflightOperation* operation,
                _Inout_ flRequestPreflight* out_preflight);
  FL_TYPE_RELEASE(RequestPreflightOperation);
};

static_assert(sizeof(flNodePrivateApi) == 3 * sizeof(void*), "Node private API layout changed");

/// Private lockstep entry point for the in-repo Node addon. Both the version and table size must match exactly.
extern "C" FL_EXPORT const flNodePrivateApi* FL_API_CALL FoundryLocalGetNodePrivateApi(
    uint32_t version, uint32_t size) FL_NO_EXCEPTION;

namespace foundry_local::detail {

class NodeAddonAccess {
 public:
  static const flSession* SessionHandle(const ChatSession& session) noexcept {
    return session.handle_.get();
  }
};

class RequestPreflightOperation {
 public:
  RequestPreflightOperation(RequestPreflightOperation&& other) noexcept
      : api_(std::exchange(other.api_, nullptr)),
        operation_(std::exchange(other.operation_, nullptr)) {}

  RequestPreflightOperation& operator=(RequestPreflightOperation&& other) noexcept {
    if (this != &other) {
      Reset();
      api_ = std::exchange(other.api_, nullptr);
      operation_ = std::exchange(other.operation_, nullptr);
    }

    return *this;
  }

  RequestPreflightOperation(const RequestPreflightOperation&) = delete;
  RequestPreflightOperation& operator=(const RequestPreflightOperation&) = delete;

  ~RequestPreflightOperation() {
    Reset();
  }

  RequestPreflight Execute() {
    flRequestPreflight preflight{};
    preflight.version = FOUNDRY_LOCAL_API_VERSION;
    Check(api_->RequestPreflightOperation_Execute(operation_, &preflight));

    return {
        preflight.prompt_tokens,
        preflight.output_reserve_tokens,
        preflight.required_tokens,
        preflight.context_limit_tokens,
        preflight.fits != 0,
        preflight.deficit_tokens,
    };
  }

 private:
  friend RequestPreflightOperation CaptureRequestPreflight(const ChatSession& session, const Request& request);

  RequestPreflightOperation(const flNodePrivateApi& api, flRequestPreflightOperation* operation) noexcept
      : api_(&api), operation_(operation) {}

  void Reset() noexcept {
    if (api_ != nullptr) {
      api_->RequestPreflightOperation_Release(operation_);
    }

    api_ = nullptr;
    operation_ = nullptr;
  }

  const flNodePrivateApi* api_ = nullptr;
  flRequestPreflightOperation* operation_ = nullptr;
};

inline RequestPreflightOperation CaptureRequestPreflight(const ChatSession& session, const Request& request) {
  static_assert(sizeof(flNodePrivateApi) <= std::numeric_limits<uint32_t>::max(),
                "flNodePrivateApi size must fit its private ABI parameter");
  const auto* api =
      FoundryLocalGetNodePrivateApi(FOUNDRY_LOCAL_NODE_PRIVATE_API_VERSION,
                                    static_cast<uint32_t>(sizeof(flNodePrivateApi)));
  if (api == nullptr) {
    throw Error("Foundry Local Node private API mismatch", FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }

  flRequestPreflightOperation* operation = nullptr;
  Check(api->Session_CaptureRequestPreflight(
      NodeAddonAccess::SessionHandle(session), request.native_handle(), &operation));
  return RequestPreflightOperation(*api, operation);
}

}  // namespace foundry_local::detail
