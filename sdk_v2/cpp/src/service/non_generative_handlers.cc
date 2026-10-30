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
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fl {

NonGenerativeRuntimeState::NonGenerativeRuntimeState(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> admission)
    : admission_(std::move(admission)) {
  if (!admission_)
    throw std::invalid_argument(
        "non-generative admission coordinator is required");
}

NonGenerativeRuntimeState::~NonGenerativeRuntimeState() = default;

std::shared_ptr<void> NonGenerativeRuntimeState::Acquire(
    const std::string& identity, const std::string& model_id, Model* owner,
    const std::function<std::shared_ptr<void>()>& factory) {
  return Acquire(identity, model_id, owner, 0, "", factory);
}

std::shared_ptr<void> NonGenerativeRuntimeState::Acquire(
    const std::string& identity, const std::string& model_id, Model* owner,
    uint64_t estimated_resident_bytes, const std::string& readiness_group,
    const std::function<std::shared_ptr<void>()>& factory) {
  std::shared_future<std::shared_ptr<void>> future;
  std::shared_ptr<std::promise<std::shared_ptr<void>>> promise;
  std::shared_ptr<NonGenerativeAdmissionReservation> reservation;
  {
    std::lock_guard lock(mutex_);
    const auto found = runtimes_.find(identity);
    if (found != runtimes_.end()) {
      future = found->second.runtime;
    } else {
      reservation = std::make_shared<NonGenerativeAdmissionReservation>(
          admission_, estimated_resident_bytes, model_id);
      promise = std::make_shared<std::promise<std::shared_ptr<void>>>();
      future = promise->get_future().share();
      runtimes_.emplace(
          identity,
          RuntimeEntry{model_id, owner, reservation, future});
    }
  }

  if (promise) {
    try {
      NonGenerativeReadinessLease readiness(admission_, readiness_group);
      promise->set_value(factory());
    } catch (...) {
      promise->set_exception(std::current_exception());
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(identity);
      if (found != runtimes_.end() &&
          found->second.reservation == reservation)
        runtimes_.erase(found);
      throw;
    }
  }
  return future.get();
}

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
  const auto package = ReadNonGenerativePackage(model->GetPath());
  if (!package) {
    throw std::invalid_argument(
        "selected catalog model is not a multi-component package");
  }
  if (package->model_id != model->Id()) {
    throw std::invalid_argument(
        "selected catalog model ID does not match package metadata");
  }
  auto provider = package->execution_provider;
  auto canonical =
      std::filesystem::weakly_canonical(model->GetPath()).string();
  return {ctx.catalog.GetName() + "|" + model->Id() + "|" + canonical + "|" +
              provider,
          model->Id(), std::move(canonical), std::move(provider),
          package->estimated_resident_bytes, model};
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
  auto state = ctx.non_generative_runtimes;
  auto runtime = std::static_pointer_cast<Runtime>(state->Acquire(
      package.identity, package.model_id, package.model,
      package.estimated_resident_bytes,
      package.provider.empty() ? "cpu" : package.provider,
      [&] {
        package.model->PrepareExternalProvider(package.provider);
        auto runtime = std::make_shared<Runtime>(package.path);
        package.model->MarkExternalRuntimeLoaded();
        return runtime;
      }));
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

std::shared_ptr<NonGenerativeAdmissionCoordinator>
CreateNonGenerativeAdmissionCoordinator(
    uint64_t memory_budget_bytes, size_t readiness_concurrency) {
  return std::make_shared<NonGenerativeAdmissionCoordinator>(
      memory_budget_bytes, readiness_concurrency);
}

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> admission) {
  return std::make_shared<NonGenerativeRuntimeState>(std::move(admission));
}

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    uint64_t memory_budget_bytes, size_t readiness_concurrency) {
  return CreateNonGenerativeRuntimeState(
      CreateNonGenerativeAdmissionCoordinator(
          memory_budget_bytes, readiness_concurrency));
}

bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model) {
  auto state = ctx.non_generative_runtimes;
  std::lock_guard lock(state->mutex_);
  auto* leaf = model.SelectedLeaf();
  leaf->UnloadExternalRuntime();
  bool removed = false;
  for (auto it = state->runtimes_.begin(); it != state->runtimes_.end();) {
    if (it->second.owner == leaf) {
      it = state->runtimes_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  return removed;
}

void ClearNonGenerativeRuntimes(ServiceContext& ctx) {
  auto state = ctx.non_generative_runtimes;
  std::lock_guard lock(state->mutex_);
  for (auto& [_, entry] : state->runtimes_) {
    if (entry.owner) entry.owner->ReleaseExternalRuntime();
  }
  state->runtimes_.clear();
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
