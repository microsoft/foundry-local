// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/models_handlers.h"

#include "catalog.h"
#include "model_info.h"
#include "service/handler_utils.h"
#include "service/web_service.h"
#include "telemetry/telemetry_action_tracker.h"

#include <foundry_local/foundry_local_c.h>  // for FOUNDRY_LOCAL_MODEL_PROP_* constants

#include <fmt/format.h>

namespace fl {

// ========================================================================
// Handler: GET /models/loaded
// ========================================================================

class ListLoadedModelsHandler : public HttpRequestHandler {
 public:
  explicit ListLoadedModelsHandler(ServiceContext& ctx) : ctx_(ctx) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    ActionTracker tracker(Action::kModelList, ctx_.telemetry,
                          InvocationContext::Direct(GetUserAgent(request)));

    auto loaded = ctx_.catalog.GetLoadedModels();
    nlohmann::json names = nlohmann::json::array();

    for (const auto* model : loaded) {
      names.push_back(model->Id());
    }

    tracker.SetStatus(ActionStatus::kSuccess);
    return JsonResponse(Status::CODE_200, names);
  }

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Handler: GET /models/load/{name}
// ========================================================================

class LoadModelHandler : public HttpRequestHandler {
 public:
  explicit LoadModelHandler(ServiceContext& ctx) : ctx_(ctx) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    ActionTracker tracker(Action::kModelLoad, ctx_.telemetry,
                          InvocationContext::Direct(GetUserAgent(request)));

    auto name_raw = request->getPathVariable("name");
    if (!name_raw) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Missing model name");
    }

    std::string name = name_raw->c_str();
    auto* model = ctx_.catalog.GetModel(name);

    if (!model) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_404, "Model not found", "No model matching '" + name + "'");
    }

    if (model->IsLoaded()) {
      tracker.SetStatus(ActionStatus::kSkipped);

      return JsonResponse(Status::CODE_200, {{"status", "already_loaded"}});
    }

    if (!model->IsCached()) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Model not cached", "Model must be downloaded before loading");
    }

    try {
      model->Load();
      tracker.SetModelId(name);
      tracker.SetStatus(ActionStatus::kSuccess);

      ctx_.logger.Log(LogLevel::Information, fmt::format("Model loaded via web service: {}", name));
      return JsonResponse(Status::CODE_200, {{"status", "loaded"}});
    } catch (const std::exception& ex) {
      tracker.SetModelId(name);
      tracker.RecordException(ex);

      ctx_.logger.Log(LogLevel::Error, fmt::format("Failed to load model {}: {}", name, ex.what()));
      return ErrorResponse(Status::CODE_500, "Load failed", ex.what());
    }
  }

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Handler: GET /models/unload/{name}
// ========================================================================

class UnloadModelHandler : public HttpRequestHandler {
 public:
  explicit UnloadModelHandler(ServiceContext& ctx) : ctx_(ctx) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    ActionTracker tracker(Action::kModelUnload, ctx_.telemetry,
                          InvocationContext::Direct(GetUserAgent(request)));

    auto name_raw = request->getPathVariable("name");
    if (!name_raw) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Missing model name");
    }

    std::string name = name_raw->c_str();
    auto* model = ctx_.catalog.GetModel(name);

    if (!model) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_404, "Model not found", "No model matching '" + name + "'");
    }

    if (!model->IsLoaded()) {
      tracker.SetStatus(ActionStatus::kSkipped);

      return JsonResponse(Status::CODE_200, {{"status", "not_loaded"}});
    }

    try {
      model->Unload();
      tracker.SetModelId(name);
      tracker.SetStatus(ActionStatus::kSuccess);

      ctx_.logger.Log(LogLevel::Information, fmt::format("Model unloaded via web service: {}", name));
      return JsonResponse(Status::CODE_200, {{"status", "unloaded"}});
    } catch (const std::exception& ex) {
      tracker.SetModelId(name);
      tracker.RecordException(ex);

      ctx_.logger.Log(LogLevel::Error, fmt::format("Failed to unload model {}: {}", name, ex.what()));
      return ErrorResponse(Status::CODE_500, "Unload failed", ex.what());
    }
  }

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Handler: GET /v1/models — OpenAI-compatible model list
// ========================================================================

class OpenAIListModelsHandler : public HttpRequestHandler {
 public:
  explicit OpenAIListModelsHandler(ServiceContext& ctx) : ctx_(ctx) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    ActionTracker tracker(Action::kOpenAIModelList, ctx_.telemetry,
                          InvocationContext::Direct(GetUserAgent(request)));

    auto models = ctx_.catalog.ListModels();
    nlohmann::json data = nlohmann::json::array();

    // List individual variants so the client knows exactly which model_id to use.
    for (const auto* model : models) {
      for (const auto* variant : model->Variants()) {
        const auto& info = variant->Info();
        int64_t created = 0;
        auto it = info.int_properties.find(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT);
        if (it != info.int_properties.end()) {
          created = it->second;
        }

        std::string owned_by = "system";
        auto pub_it = info.string_properties.find(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR);
        if (pub_it != info.string_properties.end()) {
          owned_by = pub_it->second;
        }

        data.push_back({
            {"id", info.model_id},
            {"object", "model"},
            {"created", created},
            {"owned_by", owned_by},
        });
      }
    }

    nlohmann::json body = {
        {"object", "list"},
        {"data", data},
    };

    tracker.SetStatus(ActionStatus::kSuccess);

    return JsonResponse(Status::CODE_200, body);
  }

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Handler: GET /v1/models/{name} — OpenAI-compatible model retrieve
// ========================================================================

class OpenAIRetrieveModelHandler : public HttpRequestHandler {
 public:
  explicit OpenAIRetrieveModelHandler(ServiceContext& ctx) : ctx_(ctx) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    ActionTracker tracker(Action::kOpenAIModelRetrieve, ctx_.telemetry,
                          InvocationContext::Direct(GetUserAgent(request)));

    auto name_raw = request->getPathVariable("name");
    if (!name_raw) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Missing model name");
    }

    std::string name = name_raw->c_str();
    auto* model = ctx_.catalog.GetModelVariant(name);

    if (!model) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_404, "Model not found", "No model matching '" + name + "'");
    }

    const auto& info = model->Info();
    int64_t created = 0;
    auto it = info.int_properties.find(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT);
    if (it != info.int_properties.end()) {
      created = it->second;
    }

    std::string owned_by = "system";
    auto pub_it = info.string_properties.find(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR);
    if (pub_it != info.string_properties.end()) {
      owned_by = pub_it->second;
    }

    nlohmann::json body = {
        {"id", info.model_id},
        {"object", "model"},
        {"created", created},
        {"owned_by", owned_by},
    };

    tracker.SetStatus(ActionStatus::kSuccess);

    return JsonResponse(Status::CODE_200, body);
  }

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Factory functions
// ========================================================================

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateListLoadedModelsHandler(ServiceContext& ctx) {
  return std::make_shared<ListLoadedModelsHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateLoadModelHandler(ServiceContext& ctx) {
  return std::make_shared<LoadModelHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateUnloadModelHandler(ServiceContext& ctx) {
  return std::make_shared<UnloadModelHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateOpenAIListModelsHandler(ServiceContext& ctx) {
  return std::make_shared<OpenAIListModelsHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateOpenAIRetrieveModelHandler(ServiceContext& ctx) {
  return std::make_shared<OpenAIRetrieveModelHandler>(ctx);
}

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
