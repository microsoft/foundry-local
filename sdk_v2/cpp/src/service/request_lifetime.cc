// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/request_lifetime.h"

#include "hosting/host_lifetime.h"
#include "service/handler_utils.h"
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/interceptor/RequestInterceptor.hpp>
#include <oatpp/web/server/interceptor/ResponseInterceptor.hpp>

namespace fl {
namespace {
constexpr const char* kActivityWork = "foundry.http.activity";

struct AdmittedResponse {
  std::shared_ptr<void> work;
  std::shared_ptr<HttpRequestHandler::OutgoingResponse> response;
};

struct RequestWork {
  std::shared_ptr<void> activity;
  std::shared_ptr<void> host;
};

class ActivityInterceptor final : public oatpp::web::server::interceptor::RequestInterceptor,
                                  public oatpp::web::server::interceptor::ResponseInterceptor {
 public:
  using IncomingRequest = HttpRequestHandler::IncomingRequest;
  using OutgoingResponse = HttpRequestHandler::OutgoingResponse;
  explicit ActivityInterceptor(std::shared_ptr<HttpActivityTracker> activity) : activity_(std::move(activity)) {}

  std::shared_ptr<OutgoingResponse> intercept(const std::shared_ptr<IncomingRequest>& request) override {
    auto work = activity_->Begin();
    if (!work) {
      return ErrorResponse(Status::CODE_503, "Web service is stopping");
    }

    request->putBundleData(kActivityWork, oatpp::Void(std::move(work)));
    return nullptr;
  }

  std::shared_ptr<OutgoingResponse> intercept(const std::shared_ptr<IncomingRequest>& request,
                                              const std::shared_ptr<OutgoingResponse>& response) override {
    const auto& bundle = request->getBundle().getAll();
    const auto work = bundle.find(kActivityWork);
    if (work == bundle.end()) {
      return response;
    }

    auto retained = std::make_shared<AdmittedResponse>();
    retained->work = work->second.getPtr();
    retained->response = response;
    return std::shared_ptr<OutgoingResponse>(retained, response.get());
  }

 private:
  const std::shared_ptr<HttpActivityTracker> activity_;
};

class SharedHostHandler final : public HttpRequestHandler {
 public:
  SharedHostHandler(std::shared_ptr<HttpRequestHandler> handler, std::shared_ptr<hosting::HostLifetime> lifetime)
      : handler_(std::move(handler)), lifetime_(std::move(lifetime)) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    auto work = std::make_shared<RequestWork>();
    if (request) {
      work->activity = request->getBundleData<oatpp::Void>(kActivityWork).getPtr();
    }
    if (lifetime_) {
      auto admission = lifetime_->TryAdmitHttpWork(std::chrono::steady_clock::now());
      if (!admission) {
        return ErrorResponse(Status::CODE_503, "Shared host has no active sponsoring SDK manager or is stopping HTTP");
      }
      work->host = std::make_shared<hosting::HostLifetime::WorkGuard>(std::move(admission));
    }

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

void TrackHttpActivity(oatpp::web::server::HttpConnectionHandler& handler,
                       std::shared_ptr<HttpActivityTracker> activity) {
  auto interceptor = std::make_shared<ActivityInterceptor>(std::move(activity));
  handler.addRequestInterceptor(interceptor);
  handler.addResponseInterceptor(interceptor);
}

std::shared_ptr<HttpRequestHandler> WithSharedHostAdmission(std::shared_ptr<HttpRequestHandler> handler,
                                                            std::shared_ptr<hosting::HostLifetime> lifetime) {
  return std::make_shared<SharedHostHandler>(std::move(handler), std::move(lifetime));
}

}  // namespace fl
#endif
