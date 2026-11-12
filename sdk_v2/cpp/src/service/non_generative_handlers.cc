// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/non_generative_handlers.h"

#include "catalog.h"
#include "contracts/non_generative.h"
#include "exception.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "inferencing/model_load_manager.h"
#include "model.h"
#include "service/handler_utils.h"
#include "service/web_service.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>

namespace fl {

namespace {
struct ResolvedPackage {
  std::string model_id;
  Model* model = nullptr;
};

class ModelNotFoundError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class RequestError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

ResolvedPackage ResolvePackage(ServiceContext& ctx, const std::string& requested,
                               const char* expected_task) {
  Model* model = ctx.catalog.GetModelVariant(requested);
  if (!model) {
    if (auto* alias = ctx.catalog.GetModel(requested)) model = alias->SelectedLeaf();
  }
  if (!model)
    throw ModelNotFoundError("model not found: " + requested);
  if (model->Info().task != expected_task) {
    throw RequestError(fmt::format(
        "model '{}' has task '{}'; expected '{}'", requested, model->Info().task,
        expected_task));
  }
  if (!model->IsCached()) {
    throw RequestError(
        "selected model package is not available locally");
  }
  return {model->Id(), model};
}

template <typename Runtime>
std::shared_ptr<Runtime> AcquireRuntime(
    const ResolvedPackage& package) {
  return std::static_pointer_cast<Runtime>(
      package.model->AcquireNonGenerativeRuntime());
}

template <typename Runtime, typename Request, bool IsRank>
class NonGenerativeHandler final : public HttpRequestHandler {
 public:
  NonGenerativeHandler(ServiceContext& ctx, const char* route,
                       const char* task)
      : ctx_(ctx), route_(route), task_(task) {}

  std::shared_ptr<OutgoingResponse> handle(
      const std::shared_ptr<IncomingRequest>& request) override {
    auto body = request->readBodyToString();
    if (!body || body->empty()) return ErrorResponse(Status::CODE_400, "Empty request body");
    Request input;
    try {
      input = nlohmann::ordered_json::parse(*body).get<Request>();
    } catch (const std::exception& error) {
      return ErrorResponse(Status::CODE_400, "Invalid request", error.what());
    }
    try {
      const auto package = ResolvePackage(ctx_, input.model, task_);
      auto runtime = AcquireRuntime<Runtime>(package);
      auto output = [&]() {
        if constexpr (IsRank) {
          auto result = runtime->Rank(input);
          result["model"] = package.model_id;
          return result;
        } else {
          return nlohmann::ordered_json{
              {"model", package.model_id},
              {"answers", runtime->Decide(input)},
              {"usage", {{"billing_units", 0}}},
          };
        }
      }();
      return JsonResponse(Status::CODE_200, output);
    } catch (const ModelNotFoundError& error) {
      return ErrorResponse(Status::CODE_404, "Model not found", error.what());
    } catch (const RequestError& error) {
      return ErrorResponse(Status::CODE_400, "Invalid request", error.what());
    } catch (const RuntimeCapacityError& error) {
      return ErrorResponse(Status::CODE_503, "Runtime busy", error.what());
    } catch (const Exception& error) {
      const auto status = StatusForException(error);
      if (status.code >= Status::CODE_500.code) {
        ctx_.logger.Log(
            LogLevel::Error,
            fmt::format("{} inference failed: {}", route_, error.what()));
      }
      return ErrorResponse(
          status,
          status.code < Status::CODE_500.code ? "Invalid request"
                                              : "Inference failed",
          error.what());
    } catch (const std::exception& error) {
      ctx_.logger.Log(LogLevel::Error,
                      fmt::format("{} inference failed: {}", route_, error.what()));
      const auto detail = std::string(error.what());
      return ErrorResponse(Status::CODE_500, "Inference failed", detail);
    }
  }

 private:
  ServiceContext& ctx_;
  const char* route_;
  const char* task_;
};

}  // namespace

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(
    ServiceContext& ctx) {
  return std::make_shared<
      NonGenerativeHandler<DecisionRuntime, NonGenerativeRequest, false>>(
      ctx, "systemone", "decision");
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx) {
  return std::make_shared<NonGenerativeHandler<RankingRuntime, RankingRequest, true>>(
      ctx, "rank", "ranking");
}

}  // namespace fl

#endif
