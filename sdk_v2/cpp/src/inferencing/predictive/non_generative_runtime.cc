// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_runtime.h"
#include "inferencing/predictive/session_executor.h"

#include <limits>
#include <utility>

namespace fl {
namespace {

using json = nlohmann::ordered_json;

std::unique_ptr<::NonGenerativeSession> CreateSession(const std::string& path) {
  auto config = OgaConfig::Create(path.c_str());
  return std::make_unique<::NonGenerativeSession>(*config);
}

json StructuredRequestJson(const NonGenerativeRequest& request) {
  json questions = json::object();
  for (const auto& [id, question] : request.questions) {
    questions[id] = {
        {"type", question.type},
        {"instructions", question.instructions},
        {"criteria", question.criteria},
    };
  }
  return {
      {"state", request.state},
      {"temperature", request.temperature},
      {"questions", std::move(questions)},
  };
}

}  // namespace

struct RankingRuntime::Impl {
  explicit Impl(const std::string& path)
      : executor([path] { return CreateSession(path); }) {}
  detail::SessionExecutor<::NonGenerativeSession> executor;
};

RankingRuntime::RankingRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path)) {}
RankingRuntime::~RankingRuntime() = default;

nlohmann::json RankingRuntime::Rank(
    const RankingRequest& request, std::function<bool()> is_canceled) {
  json candidates = json::object();
  for (size_t i = 0; i < request.answers.size(); ++i)
    candidates[std::to_string(i)] = request.answers[i];
  const auto request_json =
      json{{"state", request.context},
           {"instructions", request.question},
           {"temperature", request.temperature},
           {"candidates", std::move(candidates)}}
          .dump();
  const auto response = impl_->executor.Invoke(
      [request_json](::NonGenerativeSession& session) {
        return session.Run(request_json);
      },
      std::move(is_canceled));
  const auto result = json::parse(response);
  json ranked = json::array();
  for (const auto& item : result.at("ranked")) {
    const std::string key = item.at("key");
    json ranked_item = json::object();
    ranked_item["rank"] = item.at("rank");
    ranked_item["id"] = key;
    ranked_item["index"] = std::stoull(key);
    ranked_item["candidate"] =
        request.answers.at(static_cast<size_t>(std::stoull(key)));
    ranked_item["prob"] = item.at("probability");
    ranked.push_back(std::move(ranked_item));
  }
  return {{"model", result.at("model")}, {"ranked", std::move(ranked)}};
}

struct DecisionRuntime::Impl {
  explicit Impl(const std::string& path)
      : executor([path] { return CreateSession(path); }) {}
  detail::SessionExecutor<::NonGenerativeSession> executor;
};

DecisionRuntime::DecisionRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path)) {}
DecisionRuntime::~DecisionRuntime() = default;

nlohmann::json DecisionRuntime::Decide(
    const NonGenerativeRequest& request,
    std::function<bool()> is_canceled) {
  const auto request_json = StructuredRequestJson(request).dump();
  const auto response = impl_->executor.Invoke(
      [request_json](::NonGenerativeSession& session) {
        return session.Run(request_json);
      },
      std::move(is_canceled));
  return json::parse(response).at("answers");
}

}  // namespace fl
