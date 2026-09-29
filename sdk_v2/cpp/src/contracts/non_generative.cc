// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/non_generative.h"

#include <algorithm>
#include <cmath>

namespace fl {
namespace {

void ValidateStructuredNumbers(const nlohmann::ordered_json& value) {
  if (value.is_number_unsigned() &&
      value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    throw nlohmann::json::other_error::create(
        501, "unsigned structured value exceeds INT64_MAX", &value);
  }
  if (value.is_array()) {
    for (const auto& item : value) ValidateStructuredNumbers(item);
  } else if (value.is_object()) {
    for (const auto& [_, item] : value.items()) ValidateStructuredNumbers(item);
  }
}

}  // namespace

void from_json(const nlohmann::ordered_json& json, NonGenerativeQuestion& question) {
  if (!json.is_object()) {
    throw nlohmann::json::type_error::create(302, "question must be an object", &json);
  }
  question.type = json.at("type").get<std::string>();
  question.criteria = json.value("criteria", nlohmann::ordered_json(nullptr));
  question.instructions = json.value("instructions", nlohmann::ordered_json(nullptr));
  ValidateStructuredNumbers(question.criteria);
  ValidateStructuredNumbers(question.instructions);
  if (question.type != "noul" && question.type != "choice" && question.type != "score") {
    throw nlohmann::json::other_error::create(501, "question type must be noul, choice, or score", &json);
  }
}

void from_json(const nlohmann::ordered_json& json, NonGenerativeRequest& request) {
  if (!json.is_object()) {
    throw nlohmann::json::type_error::create(302, "request must be an object", &json);
  }
  request.state = json.value("state", nlohmann::ordered_json(nullptr));
  ValidateStructuredNumbers(request.state);
  const auto& questions = json.at("questions");
  if (!questions.is_object()) {
    throw nlohmann::ordered_json::type_error::create(
        302, "questions must be a non-empty object", &json);
  }
  request.questions.clear();
  for (auto it = questions.begin(); it != questions.end(); ++it)
    request.questions.emplace_back(it.key(), it->get<NonGenerativeQuestion>());
  if (request.questions.empty()) {
    throw nlohmann::json::other_error::create(501, "questions must be a non-empty object", &json);
  }
  request.model = json.value("model", std::string{"kev"});
  if (request.model.empty()) {
    throw nlohmann::json::other_error::create(501, "model must not be empty", &json);
  }
  request.temperature = json.value("temperature", 1.0f);
  if (!std::isfinite(request.temperature) || request.temperature <= 0.0f ||
      request.temperature > 100.0f) {
    throw nlohmann::json::other_error::create(501, "temperature must be in (0, 100]", &json);
  }
}

void from_json(const nlohmann::ordered_json& json, RankingRequest& request) {
  if (!json.is_object()) {
    throw nlohmann::json::type_error::create(302, "request must be an object", &json);
  }
  request.context = json.value("context", nlohmann::ordered_json(nullptr));
  ValidateStructuredNumbers(request.context);
  request.question = json.value("question", std::string{});
  request.answers = json.at("answers").get<std::vector<std::string>>();
  request.model = json.value("model", std::string{"clm"});
  request.temperature = json.value("temperature", 1.0f);
  if (request.answers.empty() ||
      std::any_of(request.answers.begin(), request.answers.end(),
                  [](const auto& answer) { return answer.empty(); })) {
    throw nlohmann::json::other_error::create(
        501, "answers must be a non-empty array of non-empty strings", &json);
  }
  if (request.model.empty()) {
    throw nlohmann::json::other_error::create(501, "model must not be empty", &json);
  }
  if (!std::isfinite(request.temperature) || request.temperature <= 0.0f ||
      request.temperature > 100.0f) {
    throw nlohmann::json::other_error::create(501, "temperature must be in (0, 100]", &json);
  }
}

}  // namespace fl
