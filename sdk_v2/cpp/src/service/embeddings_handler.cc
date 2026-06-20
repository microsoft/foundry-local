// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/embeddings_handler.h"

#include <fmt/format.h>

#include <nlohmann/json.hpp>

#include "catalog.h"
#include "contracts/embeddings.h"
#include "exception.h"
#include "inferencing/generative/embeddings/embeddings_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "inferencing/session/session_registration.h"
#include "items/tensor_item.h"
#include "items/text_item.h"
#include "model.h"
#include "service/web_service.h"
#include "telemetry/telemetry_action_tracker.h"

namespace fl {

class EmbeddingsHandler : public HttpRequestHandler {
 public:
  explicit EmbeddingsHandler(ServiceContext& ctx) : ctx_(ctx) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override {
    auto route_ctx = InvocationContext::Direct(GetUserAgent(request));
    ActionTracker tracker(Action::kOpenAIEmbeddings, ctx_.telemetry, route_ctx);

    auto body_str = request->readBodyToString();
    if (!body_str || body_str->empty()) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Empty request body");
    }

    // 1. Parse request
    EmbeddingCreateRequest req;
    try {
      auto j = nlohmann::json::parse(*body_str);
      req = j.get<EmbeddingCreateRequest>();
    } catch (const nlohmann::json::exception& ex) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Invalid JSON", ex.what());
    }

    // 2. Collect input strings
    std::vector<std::string> inputs;
    if (auto* single = std::get_if<std::string>(&req.input)) {
      inputs.push_back(*single);
    } else {
      inputs = std::get<std::vector<std::string>>(req.input);
    }

    if (inputs.empty()) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "\"input\" must not be empty");
    }

    // 3. Resolve model
    std::string model_name = req.model;
    auto* model = ctx_.catalog.GetModelVariant(model_name);
    if (!model) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_404, "Model not found", model_name);
    }

    auto* loaded = ctx_.model_load_manager.GetLoadedModel(model->Id());
    if (!loaded) {
      tracker.SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Model not loaded", model_name);
    }

    tracker.SetModelId(model_name);

    // The session and the inference it drives are indirect children of this route.
    auto session_ctx = route_ctx.AsIndirect();

    // 4. Create session and process each input
    try {
      EmbeddingsSession session(*model, *loaded, ctx_.logger, ctx_.telemetry);
      {
        ActionTracker create_tracker(Action::kSessionCreate, ctx_.telemetry, session_ctx);
        create_tracker.SetModelId(model_name);
        create_tracker.SetStatus(ActionStatus::kSuccess);
      }
      session.SetRequestContext(session_ctx);
      SessionRegistration reg(ctx_.session_manager, session);

      Request session_request;
      for (const auto& text : inputs) {
        session_request.AddOwnedItem(std::make_unique<TextItem>(text));
      }

      fl::Response session_response;
      session.ProcessRequest(session_request, session_response);

      // 5. Build response
      EmbeddingCreateResponse output;
      output.model = model_name;

      for (size_t i = 0; i < session_response.items.size(); i++) {
        if (session_response.items[i]->type != FOUNDRY_LOCAL_ITEM_TENSOR) {
          continue;
        }

        auto& tensor_item = static_cast<TensorItem&>(*session_response.items[i]);

        if (tensor_item.data_type != FOUNDRY_LOCAL_TENSOR_FLOAT) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
                   fmt::format("embedding tensor has unsupported element type {} (expected float32 = {})",
                               static_cast<int>(tensor_item.data_type),
                               static_cast<int>(FOUNDRY_LOCAL_TENSOR_FLOAT)));
        }

        // Compute element count from shape
        int64_t num_elements = 1;
        for (auto d : tensor_item.shape) {
          num_elements *= d;
        }

        EmbeddingData entry;
        entry.index = static_cast<int>(i);
        entry.embedding = std::vector<float>(
            static_cast<const float*>(tensor_item.data),
            static_cast<const float*>(tensor_item.data) + num_elements);
        output.data.push_back(std::move(entry));
      }

      // Token usage reporting is not implemented for embeddings — local inference has no
      // token budget, and counting would require a redundant tokenizer pass per input.
      // Clients that depend on these fields should treat zero as "not reported".
      output.usage.prompt_tokens = 0;
      output.usage.total_tokens = 0;

      tracker.SetStatus(ActionStatus::kSuccess);
      return JsonResponse(Status::CODE_200, nlohmann::json(output));
    } catch (const std::exception& ex) {
      tracker.RecordException(ex);
      ctx_.logger.Log(LogLevel::Error, fmt::format("Embeddings inference failed: {}", ex.what()));
      return ErrorResponse(Status::CODE_500, "Inference failed", ex.what());
    }
  }

 private:
  ServiceContext& ctx_;
};

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateEmbeddingsHandler(ServiceContext& ctx) {
  return std::make_shared<EmbeddingsHandler>(ctx);
}

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
