// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_runtime.h"
#include "catalog/non_generative_package.h"
#include "utils.h"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fl {
namespace {

using json = nlohmann::ordered_json;

std::vector<std::string> EnvironmentProviders() {
  if (const auto value = Utils::GetEnv("FOUNDRY_LOCAL_NON_GENERATIVE_PROVIDER");
      value && !value->empty()) {
    const auto provider = NormalizeNonGenerativeProvider(*value);
    return provider.empty() ? std::vector<std::string>{}
                            : std::vector<std::string>{provider};
  }
  return {};
}

std::vector<std::string> ExplicitProviders(const std::string& provider) {
  return provider.empty() ? std::vector<std::string>{}
                          : std::vector<std::string>{provider};
}

size_t CacheSetting(const char* name, size_t fallback) {
  const auto value = Utils::GetEnv(name);
  if (!value || value->empty()) return fallback;
  size_t parsed{};
  const std::string_view text(*value);
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument(std::string(name) +
                                " must be a non-negative integer");
  }
  return parsed;
}

OgaStructuredValue ToStructured(const json& value) {
  if (value.is_null()) return {};
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number_integer()) return value.get<int64_t>();
  if (value.is_number_unsigned()) {
    const auto number = value.get<uint64_t>();
    if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      throw std::invalid_argument("unsigned structured value exceeds INT64_MAX");
    return static_cast<int64_t>(number);
  }
  if (value.is_number_float()) return value.get<double>();
  if (value.is_string()) return value.get<std::string>();
  if (value.is_array()) {
    OgaStructuredValue::Array result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(ToStructured(item));
    return result;
  }
  if (value.is_object()) {
    OgaStructuredValue::Object result;
    result.reserve(value.size());
    for (auto item = value.begin(); item != value.end(); ++item)
      result.emplace_back(item.key(), ToStructured(*item));
    return result;
  }
  throw std::invalid_argument("unsupported structured JSON value");
}

OgaStructuredRequest ToRequest(const NonGenerativeRequest& value) {
  OgaStructuredRequest result;
  result.state = ToStructured(value.state);
  result.temperature = value.temperature;
  for (const auto& [id, source] : value.questions) {
    OgaQuestion question;
    question.type = source.type;
    question.instructions = ToStructured(source.instructions);
    question.criteria = ToStructured(source.criteria);
    result.questions.emplace_back(id, std::move(question));
  }
  return result;
}

json ToJson(const OgaAnswer& answer) {
  json result{{"type", answer.type}};
  if (answer.noul) result["noul"] = *answer.noul;
  if (answer.choice) result["choice"] = *answer.choice;
  if (answer.score) result["score"] = *answer.score;
  if (answer.confidence) result["confidence"] = *answer.confidence;
  if (!answer.probabilities.empty()) {
    result["probabilities"] = json::object();
    for (const auto& [key, value] : answer.probabilities)
      result["probabilities"][key] = value;
  }
  if (!answer.legend.empty()) {
    result["legend"] = json::object();
    for (const auto& [key, value] : answer.legend) result["legend"][key] = value;
  }
  return result;
}

json ToJson(const OgaModelResult& value) {
  json result = json::object();
  for (const auto& [id, answer] : value.answers) result[id] = ToJson(answer);
  return result;
}

}  // namespace

struct RankingRuntime::Impl {
  Impl(const std::string& path, std::vector<std::string> providers)
      : session(path, providers) {
    session.SetCacheCapacity(
        CacheSetting("FOUNDRY_LOCAL_CLM_CACHE_CAPACITY", 256),
        CacheSetting("FOUNDRY_LOCAL_CLM_CACHE_CAPACITY_BYTES", 64 * 1024 * 1024));
  }
  RankingSession session;
};

RankingRuntime::RankingRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path, EnvironmentProviders())) {}
RankingRuntime::RankingRuntime(const std::string& package_path, std::string provider)
    : impl_(std::make_unique<Impl>(package_path, ExplicitProviders(provider))) {}
RankingRuntime::~RankingRuntime() = default;

nlohmann::json RankingRuntime::Rank(const NonGenerativeRequest& request) {
  return ToJson(impl_->session.Run(ToRequest(request)));
}

nlohmann::json RankingRuntime::Rank(const RankingRequest& request) {
  OgaFreeFormRankRequest translated;
  translated.state = ToStructured(request.context);
  translated.instructions = request.question;
  translated.temperature = request.temperature;
  for (size_t i = 0; i < request.answers.size(); ++i)
    translated.candidates.emplace_back(std::to_string(i), request.answers[i]);
  const auto result = impl_->session.Rank(translated);
  json ranked = json::array();
  for (const auto& item : result.ranked) {
    ranked.push_back({
        {"rank", item.rank},
        {"candidate", request.answers.at(static_cast<size_t>(std::stoull(item.key)))},
        {"prob", item.probability},
    });
  }
  return {{"model", result.model}, {"ranked", std::move(ranked)}};
}

struct DecisionRuntime::Impl {
  Impl(const std::string& path, std::vector<std::string> providers)
      : session(path, providers) {
    session.SetCacheCapacity(
        CacheSetting("FOUNDRY_LOCAL_KEV_CACHE_CAPACITY", 512),
        CacheSetting("FOUNDRY_LOCAL_KEV_CACHE_CAPACITY_BYTES", 16 * 1024 * 1024));
  }
  DecisionSession session;
};

DecisionRuntime::DecisionRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path, EnvironmentProviders())) {}
DecisionRuntime::DecisionRuntime(const std::string& package_path, std::string provider)
    : impl_(std::make_unique<Impl>(package_path, ExplicitProviders(provider))) {}
DecisionRuntime::~DecisionRuntime() = default;

nlohmann::json DecisionRuntime::Decide(const NonGenerativeRequest& request) {
  return ToJson(impl_->session.Decide(ToRequest(request)));
}

}  // namespace fl
