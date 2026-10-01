// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_session.h"

#include "catalog/non_generative_package.h"
#include "contracts/non_generative.h"
#include "exception.h"
#include "items/text_item.h"
#include "model.h"

#include <nlohmann/json.hpp>

namespace fl {

NonGenerativeSession::NonGenerativeSession(
    const Model& model, ILogger& logger, ITelemetry& telemetry)
    : Session(model, logger, telemetry) {
  model_ = const_cast<Model&>(model).AcquireExternalSession();
  try {
    model_id_ = model_->Id();
    if (model_->Info().execution_provider.empty()) {
      const auto package = ReadNonGenerativePackage(model_->GetPath());
      if (package) provider_ = package->execution_provider;
    } else {
      provider_ = NormalizeNonGenerativeProvider(
          model_->Info().execution_provider);
    }
    if (model_->Info().task == "text-ranking") {
      ranking_ =
          std::make_unique<RankingRuntime>(model_->GetPath(), provider_);
    } else if (model_->Info().task == "typed-decision") {
      decision_ =
          std::make_unique<DecisionRuntime>(model_->GetPath(), provider_);
    } else {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "unsupported non-generative model task: ",
               model_->Info().task);
    }
  } catch (...) {
    model_->ReleaseExternalSession();
    if (model_->ActiveExternalSessionCount() == 0)
      model_->UnloadExternalRuntime();
    model_ = nullptr;
    throw;
  }
}

NonGenerativeSession::~NonGenerativeSession() {
  ranking_.reset();
  decision_.reset();
  if (!model_) return;
  model_->ReleaseExternalSession();
  if (model_->ActiveExternalSessionCount() == 0) {
    try {
      model_->UnloadExternalRuntime();
    } catch (...) {
    }
  }
}

void NonGenerativeSession::ProcessRequestImpl(
    const Request& request, Response& response) {
  if (request.items.size() != 1 ||
      request.items.front()->type != FOUNDRY_LOCAL_ITEM_TEXT) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "non-generative requests require one JSON text item");
  }
  const auto& item =
      static_cast<const TextItem&>(*request.items.front());
  if (item.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "non-generative requests require an OPENAI_JSON text item");
  }

  nlohmann::json result;
  if (ranking_) {
    RankingRequest parsed;
    try {
      parsed = nlohmann::ordered_json::parse(item.text)
                   .get<RankingRequest>();
    } catch (const nlohmann::json::exception& error) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "invalid ranking request: ", error.what());
    }
    result = ranking_->Rank(parsed);
    result["model"] = model_id_;
  } else {
    NonGenerativeRequest parsed;
    try {
      parsed = nlohmann::ordered_json::parse(item.text)
                   .get<NonGenerativeRequest>();
    } catch (const nlohmann::json::exception& error) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "invalid decision request: ", error.what());
    }
    result = {
        {"model", model_id_},
        {"answers", decision_->Decide(parsed)},
        {"usage", {{"billing_units", 0}}},
    };
  }
  response.items.push_back(std::make_unique<TextItem>(
      result.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
}

}  // namespace fl
