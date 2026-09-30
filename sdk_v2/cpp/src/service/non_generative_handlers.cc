// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/non_generative_handlers.h"

#include "catalog.h"
#include "catalog/non_generative_package.h"
#include "contracts/non_generative.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "model.h"
#include "service/handler_utils.h"
#include "service/web_service.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fl {

struct RuntimeEntry {
  std::string model_id;
  Model* owner;
  std::shared_future<std::shared_ptr<void>> runtime;
  std::shared_ptr<void> reservation;
};

class NonGenerativeRuntimeState {
 public:
  std::mutex mutex;
  std::map<std::string, RuntimeEntry> runtimes;
};

namespace {
struct ResolvedPackage {
  std::string identity;
  std::string model_id;
  std::string path;
  std::string provider;
  Model* model = nullptr;
};

class ModelNotFoundError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string ConfiguredPackagePath(const char* specific, const char* directory) {
  if (const char* value = std::getenv(specific); value && *value) return value;
  if (const char* root = std::getenv("FOUNDRY_LOCAL_NON_GENERATIVE_PACKAGE_ROOT"); root && *root)
    return (std::filesystem::path(root) / directory).string();
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
            std::move(canonical), std::move(provider), model};
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
  if (const char* value = std::getenv("FOUNDRY_LOCAL_NON_GENERATIVE_PROVIDER");
      value && *value) {
    provider = NormalizeNonGenerativeProvider(value);
  }
  const auto model_id = metadata ? metadata->model_id : requested;
  return {"prototype|" + model_id + "|" + canonical + "|" + provider, model_id,
          std::move(canonical), std::move(provider), nullptr};
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
    std::shared_future<std::shared_ptr<void>> future;
    std::shared_ptr<std::promise<std::shared_ptr<void>>> promise;
    std::shared_ptr<void> reservation;
    {
      std::lock_guard lock(state->mutex);
      const auto found = state->runtimes.find(package.identity);
      if (found != state->runtimes.end()) {
        future = found->second.runtime;
      } else {
        promise = std::make_shared<std::promise<std::shared_ptr<void>>>();
        future = promise->get_future().share();
        reservation = std::make_shared<int>(0);
        state->runtimes.emplace(
            package.identity,
            RuntimeEntry{package.model_id, package.model, future, reservation});
      }
    }

    if (promise) {
      try {
        promise->set_value(
            std::make_shared<Runtime>(package.path, package.provider));
      } catch (...) {
        promise->set_exception(std::current_exception());
        std::lock_guard lock(state->mutex);
        const auto found = state->runtimes.find(package.identity);
        if (found != state->runtimes.end() &&
            found->second.reservation == reservation) {
          state->runtimes.erase(found);
        }
        throw;
      }
    }

    return {
        std::static_pointer_cast<Runtime>(future.get()),
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
      const auto package = ResolvePackage(ctx_, input.model, task_, environment_, directory_);
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

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState() {
  return std::make_shared<NonGenerativeRuntimeState>();
}

bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model) {
  auto state = ctx.non_generative_runtimes;
  std::lock_guard lock(state->mutex);
  auto* leaf = model.SelectedLeaf();
  leaf->UnloadExternalRuntime();
  bool removed = false;
  for (auto it = state->runtimes.begin(); it != state->runtimes.end();) {
    if (it->second.owner == leaf) {
      it = state->runtimes.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  return removed;
}

void ClearNonGenerativeRuntimes(ServiceContext& ctx) {
  auto state = ctx.non_generative_runtimes;
  std::lock_guard lock(state->mutex);
  for (auto& [_, entry] : state->runtimes) {
    if (entry.owner) entry.owner->UnloadExternalRuntime();
  }
  state->runtimes.clear();
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
