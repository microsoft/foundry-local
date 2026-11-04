// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/request_lifetime.h"

#include "hosting/host_lifetime.h"
#include "service/handler_utils.h"

namespace fl {
namespace {

struct AdmittedResponse {
  std::shared_ptr<void> work;
  std::shared_ptr<HttpRequestHandler::OutgoingResponse> response;
};

class SharedHostHandler final : public HttpRequestHandler {
 public:
  SharedHostHandler(std::shared_ptr<HttpRequestHandler> handler, std::shared_ptr<hosting::HostLifetime> lifetime)
      : handler_(std::move(handler)), lifetime_(std::move(lifetime)) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    auto admission = lifetime_->TryAdmitHttpWork(std::chrono::steady_clock::now());
    if (!admission) {
      return ErrorResponse(Status::CODE_503, "Shared host has no active sponsoring SDK manager or is stopping HTTP");
    }

    auto work = std::make_shared<hosting::HostLifetime::WorkGuard>(std::move(admission));
    HttpRequestLifetime scope(work);
    auto retained = std::make_shared<AdmittedResponse>();
    retained->work = std::move(work);
    retained->response = handler_->handle(request);
    // Keep the original response, including body/headers/bundle, through synchronous or asynchronous delivery.
    return std::shared_ptr<OutgoingResponse>(retained, retained->response.get());
  }

 private:
  const std::shared_ptr<HttpRequestHandler> handler_;
  const std::shared_ptr<hosting::HostLifetime> lifetime_;
};

}  // namespace

std::shared_ptr<HttpRequestHandler> WithSharedHostAdmission(std::shared_ptr<HttpRequestHandler> handler,
                                                            std::shared_ptr<hosting::HostLifetime> lifetime) {
  return lifetime ? std::make_shared<SharedHostHandler>(std::move(handler), std::move(lifetime)) : std::move(handler);
}

}  // namespace fl
#endif
