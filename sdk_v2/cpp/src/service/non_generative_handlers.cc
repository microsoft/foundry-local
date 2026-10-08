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
#include "utils.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fl {

NonGenerativeRuntimeState::NonGenerativeRuntimeState(
    uint64_t memory_budget_bytes, size_t readiness_concurrency)
    : memory_budget_bytes_(memory_budget_bytes),
      readiness_concurrency_(readiness_concurrency) {
  if (!memory_budget_bytes_)
    throw std::invalid_argument(
        "non-generative memory budget must be positive");
  if (!readiness_concurrency_)
    throw std::invalid_argument(
        "non-generative readiness concurrency must be positive");
}

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
  std::shared_ptr<void> reservation;
  {
    std::lock_guard lock(mutex_);
    const auto found = runtimes_.find(identity);
    if (found != runtimes_.end()) {
      future = found->second.runtime;
    } else {
      if (estimated_resident_bytes >
          memory_budget_bytes_ - reserved_bytes_)
        throw std::runtime_error(fmt::format(
            "loading '{}' requires {} bytes but only {} bytes remain in the "
            "non-generative runtime budget",
            model_id, estimated_resident_bytes,
            memory_budget_bytes_ - reserved_bytes_));
      promise = std::make_shared<std::promise<std::shared_ptr<void>>>();
      future = promise->get_future().share();
      reservation = std::make_shared<int>(0);
      reserved_bytes_ += estimated_resident_bytes;
      runtimes_.emplace(
          identity, RuntimeEntry{model_id, owner, future, reservation,
                                 estimated_resident_bytes});
    }
  }

  if (promise) {
    {
      std::unique_lock readiness_lock(readiness_mutex_);
      readiness_condition_.wait(readiness_lock, [&] {
        return active_readiness_[readiness_group] <
               readiness_concurrency_;
      });
      ++active_readiness_[readiness_group];
    }
    const auto release_readiness = [&] {
      {
        std::lock_guard readiness_lock(readiness_mutex_);
        auto found = active_readiness_.find(readiness_group);
        if (found != active_readiness_.end() && --found->second == 0)
          active_readiness_.erase(found);
      }
      readiness_condition_.notify_all();
    };
    try {
      promise->set_value(factory());
      release_readiness();
    } catch (...) {
      release_readiness();
      promise->set_exception(std::current_exception());
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(identity);
      if (found != runtimes_.end() &&
          found->second.reservation == reservation) {
        reserved_bytes_ -= found->second.resource_bytes;
        runtimes_.erase(found);
      }
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

std::string ConfiguredPackagePath(const char* specific, const char* directory) {
  if (const auto value = Utils::GetEnv(specific); value && !value->empty()) return *value;
  if (const auto root = Utils::GetEnv("FOUNDRY_LOCAL_NON_GENERATIVE_PACKAGE_ROOT");
      root && !root->empty())
    return (std::filesystem::path(*root) / directory).string();
  throw std::runtime_error(fmt::format(
      "{} is not configured; register the requested package in the local catalog, set {}, "
      "or set FOUNDRY_LOCAL_NON_GENERATIVE_PACKAGE_ROOT with {}/ beneath it",
      directory, specific, directory));
}

ResolvedPackage ResolvePackage(ServiceContext& ctx, const std::string& requested,
                               const char* expected_task, const char* environment,
                               const char* directory) {
  Model* model = ctx.catalog.GetModelVariant(requested);
  if (!model) {
    if (auto* alias = ctx.catalog.GetModel(requested)) model = alias->SelectedLeaf();
  }
  if (model) {
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
      throw std::invalid_argument("selected catalog model is not a multi-component package");
    }
    if (package->model_id != model->Id()) {
      throw std::invalid_argument(
          "selected catalog model ID does not match package metadata");
    }
    auto provider = model->Info().execution_provider.empty()
                        ? package->execution_provider
                        : NormalizeNonGenerativeProvider(model->Info().execution_provider);
    auto canonical = std::filesystem::weakly_canonical(model->GetPath()).string();
    return {ctx.catalog.GetName() + "|" + model->Id() + "|" + canonical + "|" + provider,
            model->Id(),
            std::move(canonical), std::move(provider),
            package->estimated_resident_bytes, model};
  }

  const bool is_rank = std::string_view(expected_task) == "text-ranking";
  const bool allowed_fallback =
      is_rank ? (requested == "clm" || requested == "clm-latest")
              : (requested == "kev" || requested == "kev-latest");
  if (!allowed_fallback) {
    throw ModelNotFoundError("model not found: " + requested);
  }

  const auto path = ConfiguredPackagePath(environment, directory);
  auto canonical = std::filesystem::weakly_canonical(path).string();
  const auto metadata = ReadNonGenerativePackage(canonical);
  if (metadata && metadata->task != expected_task) {
    throw std::invalid_argument(fmt::format(
        "fallback package has task '{}'; expected '{}'", metadata->task, expected_task));
  }
  const std::string_view expected_alias = is_rank ? "clm" : "kev";
  if (metadata && metadata->alias != expected_alias) {
    throw std::invalid_argument(fmt::format(
        "fallback package has alias '{}'; expected '{}'", metadata->alias,
        expected_alias));
  }
  std::string provider = metadata ? metadata->execution_provider : std::string{};
  if (const auto value = Utils::GetEnv("FOUNDRY_LOCAL_NON_GENERATIVE_PROVIDER");
      value && !value->empty()) {
    provider = NormalizeNonGenerativeProvider(*value);
  }
  const auto model_id = metadata ? metadata->model_id : requested;
  return {"prototype|" + model_id + "|" + canonical + "|" + provider, model_id,
          std::move(canonical), std::move(provider),
          metadata ? metadata->estimated_resident_bytes : 0, nullptr};
}

class ModelSessionLease {
 public:
  struct AlreadyAcquired {};
  explicit ModelSessionLease(Model* model, AlreadyAcquired) : model_(model) {}
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
  Model* acquired = package.model ? package.model->AcquireExternalSession() : nullptr;
  auto lease = acquired
                   ? std::make_unique<ModelSessionLease>(
                         acquired, ModelSessionLease::AlreadyAcquired{})
                   : nullptr;
  auto state = ctx.non_generative_runtimes;
  try {
    ctx.model_load_manager.PrepareNonGenerativeProvider(package.provider);
    return {
        std::static_pointer_cast<Runtime>(state->Acquire(
            package.identity, package.model_id, package.model,
            package.estimated_resident_bytes,
            package.provider.empty() ? "cpu" : package.provider,
            [&] {
              return std::make_shared<Runtime>(package.path, package.provider);
            })),
        std::move(lease),
    };
  } catch (...) {
    lease.reset();
    if (acquired && acquired->ActiveExternalSessionCount() == 0) {
      try {
        acquired->UnloadExternalRuntime();
      } catch (const std::exception& error) {
        ctx.logger.Log(
            LogLevel::Warning,
            fmt::format("failed to clear non-generative load state for {}: {}",
                        package.model_id, error.what()));
      }
    }
    throw;
  }
}

template <typename Runtime, typename Request, bool IsRank>
class NonGenerativeHandler final : public HttpRequestHandler {
 public:
  NonGenerativeHandler(ServiceContext& ctx, const char* environment,
                       const char* directory, const char* task)
      : ctx_(ctx), environment_(environment), directory_(directory), task_(task) {}

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
      const auto package = ResolvePackage(
          ctx_, input.model, task_, environment_, directory_);
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
                      fmt::format("{} inference failed: {}", directory_, error.what()));
      const auto detail = std::string(error.what());
      if (detail.find("is not configured") != std::string::npos)
        return ErrorResponse(Status::CODE_503, "Model package not configured", detail);
      return ErrorResponse(Status::CODE_500, "Inference failed", detail);
    }
  }

 private:
  ServiceContext& ctx_;
  const char* environment_;
  const char* directory_;
  const char* task_;
};

}  // namespace

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    uint64_t memory_budget_bytes, size_t readiness_concurrency) {
  return std::make_shared<NonGenerativeRuntimeState>(
      memory_budget_bytes, readiness_concurrency);
}

bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model) {
  auto state = ctx.non_generative_runtimes;
  std::lock_guard lock(state->mutex_);
  auto* leaf = model.SelectedLeaf();
  leaf->UnloadExternalRuntime();
  bool removed = false;
  for (auto it = state->runtimes_.begin(); it != state->runtimes_.end();) {
    if (it->second.owner == leaf) {
      state->reserved_bytes_ -= it->second.resource_bytes;
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
    if (entry.owner) entry.owner->UnloadExternalRuntime();
  }
  state->runtimes_.clear();
  state->reserved_bytes_ = 0;
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(
    ServiceContext& ctx) {
  return std::make_shared<
      NonGenerativeHandler<DecisionRuntime, NonGenerativeRequest, false>>(
      ctx, "FOUNDRY_LOCAL_SYSTEMONE_MODEL_PATH", "systemone", "typed-decision");
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx) {
  return std::make_shared<NonGenerativeHandler<RankingRuntime, RankingRequest, true>>(
      ctx, "FOUNDRY_LOCAL_RANK_MODEL_PATH", "rank", "text-ranking");
}

}  // namespace fl

#endif
