// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/non_generative_handlers.h"

#include "catalog.h"
#include "catalog/non_generative_package.h"
#include "contracts/non_generative.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "inferencing/model_load_manager.h"
#include "model.h"
#include "service/handler_utils.h"
#include "service/web_service.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace fl {

namespace {
struct ResolvedPackage {
  std::string identity;
  std::string model_id;
  std::string path;
  std::string provider;
  uint64_t estimated_resident_bytes{};
  Model* model = nullptr;
};

class ModelNotFoundError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

NonGenerativePackageMetadata CachedPackageMetadata(
    const std::string& path) {
  static std::mutex mutex;
  static std::unordered_map<std::string, NonGenerativePackageMetadata> cache;
  const auto canonical =
      std::filesystem::weakly_canonical(path).string();
  std::lock_guard lock(mutex);
  if (const auto found = cache.find(canonical); found != cache.end())
    return found->second;
  const auto package = ReadNonGenerativePackage(canonical);
  if (!package)
    throw std::invalid_argument(
        "selected catalog model is not a multi-component package");
  return cache.emplace(canonical, *package).first->second;
}

ResolvedPackage ResolvePackage(ServiceContext& ctx, const std::string& requested,
                               const char* expected_task) {
  Model* model = ctx.catalog.GetModelVariant(requested);
  if (!model) {
    if (auto* alias = ctx.catalog.GetModel(requested)) model = alias->SelectedLeaf();
  }
  if (!model)
    throw ModelNotFoundError("model not found: " + requested);
  if (model->Info().task != expected_task) {
    throw std::invalid_argument(fmt::format(
        "model '{}' has task '{}'; expected '{}'", requested, model->Info().task,
        expected_task));
  }
  if (!model->IsCached()) {
    throw std::invalid_argument("selected model package is not available locally");
  }
  const auto package = CachedPackageMetadata(model->GetPath());
  auto provider = package.execution_provider;
  auto canonical =
      std::filesystem::weakly_canonical(model->GetPath()).string();
  return {ctx.catalog.GetName() + "|" + model->Id() + "|" + canonical + "|" +
              provider,
          model->Id(), std::move(canonical), std::move(provider),
          package.estimated_resident_bytes, model};
}

class ModelSessionLease {
 public:
  explicit ModelSessionLease(Model* model) : model_(model) {}
  ~ModelSessionLease() {
    if (model_) model_->ReleaseExternalSession();
  }
  ModelSessionLease(const ModelSessionLease&) = delete;
  ModelSessionLease& operator=(const ModelSessionLease&) = delete;

 private:
  Model* model_;
};

template <typename Runtime>
std::pair<std::shared_ptr<Runtime>, std::unique_ptr<ModelSessionLease>>
AcquireRuntime(ServiceContext& ctx, const ResolvedPackage& package) {
  Model* acquired = package.model->AcquireExternalSession();
  auto lease = std::make_unique<ModelSessionLease>(acquired);
  auto runtime = std::static_pointer_cast<Runtime>(
      ctx.model_load_manager.AcquireNonGenerativeRuntime(
          *package.model, package.model->Info().task, package.provider,
          package.estimated_resident_bytes));
  return {std::move(runtime), std::move(lease)};
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
    } catch (const nlohmann::json::exception& error) {
      return ErrorResponse(Status::CODE_400, "Invalid request", error.what());
    }
    try {
      const auto package = ResolvePackage(ctx_, input.model, task_);
      auto [runtime, lease] = AcquireRuntime<Runtime>(ctx_, package);
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
    } catch (const std::invalid_argument& error) {
      return ErrorResponse(Status::CODE_400, "Invalid request", error.what());
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

bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model) {
  return ctx.model_load_manager.UnloadNonGenerativeRuntime(model);
}

void ClearNonGenerativeRuntimes(ServiceContext& ctx) {
  ctx.model_load_manager.NonGenerativeRuntimes()->Clear();
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(
    ServiceContext& ctx) {
  return std::make_shared<
      NonGenerativeHandler<DecisionRuntime, NonGenerativeRequest, false>>(
      ctx, "systemone", "typed-decision");
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx) {
  return std::make_shared<NonGenerativeHandler<RankingRuntime, RankingRequest, true>>(
      ctx, "rank", "text-ranking");
}

}  // namespace fl

#endif
