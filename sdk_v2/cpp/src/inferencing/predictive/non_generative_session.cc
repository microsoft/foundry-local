// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_session.h"

#include "catalog/non_generative_package.h"
#include "contracts/non_generative.h"
#include "exception.h"
#include "items/text_item.h"
#include "model.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string_view>

namespace fl {
namespace {

float ParseTemperature(const char* value) {
  try {
    size_t consumed{};
    const auto parsed = std::stof(value, &consumed);
    if (consumed != std::string_view(value).size() ||
        !std::isfinite(parsed))
      throw std::invalid_argument("invalid temperature");
    return parsed;
  } catch (const std::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "temperature option must be a finite number");
  }
}

void ApplyOptions(const KeyValuePairs& options,
                  nlohmann::ordered_json& payload) {
  for (const auto* key : options.Keys()) {
    if (std::string_view(key) != "temperature")
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "unsupported non-generative option: ", key);
  }
  if (!payload.contains("temperature")) {
    if (const auto* temperature = options.Find("temperature"))
      payload["temperature"] = ParseTemperature(temperature);
  }
}

void ValidateBoundModel(const nlohmann::ordered_json& payload,
                        const Model& model) {
  if (!payload.contains("model")) return;
  const auto selected = payload.at("model").get<std::string>();
  if (selected != model.Id() && selected != model.Info().alias)
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "request model does not match the bound session model");
}

}  // namespace

NonGenerativeSession::NonGenerativeSession(
    const Model& model, ILogger& logger, ITelemetry& telemetry)
    : Session(model, logger, telemetry) {
  model_ = const_cast<Model&>(model).AcquireExternalSession();
  try {
    model_id_ = model_->Id();
    const auto package = ReadNonGenerativePackage(model_->GetPath());
    if (!package)
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "model is not a non-generative package");
    provider_ = package->execution_provider;
    auto runtime = model_->AcquireExternalRuntime();
    if (model_->Info().task == "text-ranking")
      ranking_ = std::static_pointer_cast<RankingRuntime>(runtime);
    else if (model_->Info().task == "typed-decision")
      decision_ = std::static_pointer_cast<DecisionRuntime>(runtime);
    else
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "unsupported non-generative model task: ",
               model_->Info().task);
  } catch (...) {
    ranking_.reset();
    decision_.reset();
    model_->ReleaseExternalSession();
    model_ = nullptr;
    throw;
  }
}

NonGenerativeSession::~NonGenerativeSession() {
  ranking_.reset();
  decision_.reset();
  if (!model_) return;
  model_->ReleaseExternalSession();
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
  if (item.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT &&
      item.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "non-generative requests require an OPENAI_JSON text item");
  }

  nlohmann::json result;
  nlohmann::ordered_json payload;
  try {
    payload = nlohmann::ordered_json::parse(item.text);
    ApplyOptions(MergedOptions(request.options), payload);
    ValidateBoundModel(payload, *model_);
  } catch (const nlohmann::json::exception& error) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "invalid non-generative request: ", error.what());
  }
  if (ranking_) {
    RankingRequest parsed;
    try {
      parsed = payload.get<RankingRequest>();
    } catch (const nlohmann::json::exception& error) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "invalid ranking request: ", error.what());
    }
    result = ranking_->Rank(parsed);
    result["model"] = model_id_;
  } else {
    NonGenerativeRequest parsed;
    try {
      parsed = payload.get<NonGenerativeRequest>();
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
