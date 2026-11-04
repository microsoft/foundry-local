// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/models_handlers.h"

#include "catalog.h"
#include "inferencing/model_load_manager.h"
#include "model_info.h"
#include "platform/path.h"
#include "service/handler_utils.h"
#include "service/web_service.h"
#include "telemetry/telemetry_action_tracker.h"
#include "util/path_safety.h"

#include <foundry_local/foundry_local_c.h>  // for FOUNDRY_LOCAL_MODEL_PROP_* constants

#include <fmt/format.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace fl {

namespace {

int HexDigitValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }

  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }

  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }

  return -1;
}

std::optional<std::string> DecodeUrlComponent(std::string_view segment, bool plus_as_space) {
  // oatpp 1.3 leaves path variables and query values encoded. Only query values use '+' as a form-space.
  std::string decoded;
  decoded.reserve(segment.size());
  for (size_t i = 0; i < segment.size(); ++i) {
    auto ch = static_cast<unsigned char>(segment[i]);
    if (ch == '+' && plus_as_space) {
      ch = ' ';
    } else if (ch == '%') {
      if (segment.size() - i < 3) {
        return std::nullopt;
      }

      const auto high = HexDigitValue(segment[i + 1]);
      const auto low = HexDigitValue(segment[i + 2]);
      if (high < 0 || low < 0) {
        return std::nullopt;
      }

      ch = static_cast<unsigned char>((high << 4) | low);
      i += 2;
    }

    if (ch < 0x20 || ch == 0x7f) {
      return std::nullopt;
    }

    decoded.push_back(static_cast<char>(ch));
  }

  return decoded;
}

std::optional<std::filesystem::path> CanonicalModelPath(const std::string& path) {
  if (path.empty()) {
    return std::nullopt;
  }

  try {
    std::filesystem::path canonical;
    std::string error;
    if (platform::GetWeaklyCanonicalPath(std::filesystem::path(path), canonical, error)) {
      return canonical;
    }
  } catch (const std::filesystem::filesystem_error&) {
    // An unrepresentable request path is invalid input, not a service failure.
  }

  return std::nullopt;
}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ValidateExpectedModelPath(
    const HttpRequestHandler::IncomingRequest& request, const std::string& model_path) {
  const auto raw = request.getQueryParameter("expected_model_path");
  if (!raw) {
    return nullptr;
  }

  const auto decoded = DecodeUrlComponent(*raw, true);
  const auto expected = decoded ? CanonicalModelPath(*decoded) : std::nullopt;
  if (!expected) {
    return ErrorResponse(Status::CODE_400, "Invalid expected_model_path",
                         "Expected a non-empty, percent-encoded filesystem path");
  }

  const auto actual = CanonicalModelPath(model_path);
  // Use the loader's identity rules; unload must still work after the artifact directory is removed.
  if (!actual || !ModelPathsEqual(actual->string(), expected->string())) {
    return ErrorResponse(Status::CODE_409, "Model path mismatch",
                         "expected_model_path does not match the server's model path");
  }

  return nullptr;
}

Model* ResolveManagedModel(ICatalog& catalog, const std::string& name) {
  if (auto* variant = catalog.GetModelVariant(name)) {
    return variant;
  }

  if (auto* model = catalog.GetModel(name)) {
    return catalog.GetModelVariant(model->Id());
  }

  return nullptr;
}

struct SharedModelState {
  std::string model_id;
  std::string model_path;
  std::optional<uint64_t> context_length;
};

void to_json(nlohmann::json& json, const SharedModelState& response) {
  json = {{"model_id", response.model_id}, {"model_path", response.model_path}};
  if (response.context_length) {
    json["context_length"] = *response.context_length;
  }
}

struct LoadModelResponse {
  std::string status;
  SharedModelState model;
};

void to_json(nlohmann::json& json, const LoadModelResponse& response) {
  json = response.model;
  json["status"] = response.status;
}

SharedModelState MakeSharedModelState(const std::string& model_id, const ModelLoadManager::LoadedModelState& state) {
  return {.model_id = model_id, .model_path = state.model_path, .context_length = state.context_length};
}

LoadModelResponse MakeLoadModelResponse(ServiceContext& ctx, const Model& model, std::string status) {
  SharedModelState response{
      .model_id = model.Id(), .model_path = NormalizeModelPath(model.GetPath()), .context_length = std::nullopt};
  const auto state = ctx.model_load_manager.GetLoadedModelState(model.Id(), model.GetPath());
  if (state) {
    response = MakeSharedModelState(model.Id(), *state);
  }

  return {.status = std::move(status), .model = std::move(response)};
}

}  // namespace

// ========================================================================
// Handler: GET /models/loaded
// ========================================================================

class ListLoadedModelsHandler : public HttpRequestHandler {
 public:
  explicit ListLoadedModelsHandler(ServiceContext& ctx) : ctx_(ctx) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    ActionTracker tracker(Action::kModelList, ctx_.telemetry,
                          InvocationContext::Direct(GetUserAgent(request)));

    const auto details = request->getQueryParameter("details");
    const bool include_details = details && *details == "true";
    auto loaded = ctx_.catalog.GetLoadedModels();
    nlohmann::json names = nlohmann::json::array();

    for (const auto* model : loaded) {
      if (include_details) {
        const auto state = ctx_.model_load_manager.GetLoadedModelState(model->Id(), model->GetPath());
        if (state) {
          names.push_back(MakeSharedModelState(model->Id(), *state));
        }
      } else {
        names.push_back(model->Id());
      }
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

    const auto decoded_name = DecodeUrlComponent(*name_raw, false);
    if (!decoded_name) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Invalid model name", "Invalid path segment encoding");
    }

    const auto& name = *decoded_name;
    auto* model = ResolveManagedModel(ctx_.catalog, name);

    if (!model) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_404, "Model not found", "No model matching '" + name + "'");
    }
    tracker.SetModelId(model->Id());

    const auto model_path = model->GetPath();
    if (auto error = ValidateExpectedModelPath(*request, model_path)) {
      tracker.SetStatus(ActionStatus::kClientError);
      return error;
    }

    if (model->IsLoaded()) {
      tracker.SetStatus(ActionStatus::kSkipped);

      return JsonResponse(Status::CODE_200, MakeLoadModelResponse(ctx_, *model, "already_loaded"));
    }

    if (!model->IsCached()) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Model not cached", "Model must be downloaded before loading");
    }

    try {
      model->Load(ExecutionProvider::kDefault, model_path);
      tracker.SetStatus(ActionStatus::kSuccess);

      ctx_.logger.Log(LogLevel::Information, fmt::format("Model loaded via web service: {}", name));
      return JsonResponse(Status::CODE_200, MakeLoadModelResponse(ctx_, *model, "loaded"));
    } catch (const std::exception& ex) {
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

    const auto decoded_name = DecodeUrlComponent(*name_raw, false);
    if (!decoded_name) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Invalid model name", "Invalid path segment encoding");
    }

    const auto& name = *decoded_name;
    auto* model = ResolveManagedModel(ctx_.catalog, name);

    if (!model) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_404, "Model not found", "No model matching '" + name + "'");
    }
    tracker.SetModelId(model->Id());

    if (auto error = ValidateExpectedModelPath(*request, model->GetPath())) {
      tracker.SetStatus(ActionStatus::kClientError);
      return error;
    }

    if (!model->IsLoaded()) {
      tracker.SetStatus(ActionStatus::kSkipped);

      return JsonResponse(Status::CODE_200, {{"status", "not_loaded"}});
    }

    try {
      model->Unload();
      tracker.SetStatus(ActionStatus::kSuccess);

      ctx_.logger.Log(LogLevel::Information, fmt::format("Model unloaded via web service: {}", name));
      return JsonResponse(Status::CODE_200, {{"status", "unloaded"}});
    } catch (const std::exception& ex) {
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
