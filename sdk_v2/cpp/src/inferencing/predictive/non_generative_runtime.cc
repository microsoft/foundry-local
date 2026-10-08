// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_runtime.h"
#include "catalog/non_generative_package.h"
#include "utils.h"

#include <charconv>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
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

template <typename Session>
class SessionExecutor {
 public:
  using Factory = std::function<std::unique_ptr<Session>()>;

  explicit SessionExecutor(Factory factory)
      : thread_([this, factory = std::move(factory)]() mutable {
          // CUDA graph capture and replay must remain on one execution thread
          // even when HTTP requests arrive on different service workers.
          try {
            session_ = factory();
          } catch (...) {
            startup_error_ = std::current_exception();
          }
          {
            std::lock_guard lock(mutex_);
            ready_ = true;
          }
          condition_.notify_all();
          if (startup_error_) return;

          std::unique_lock lock(mutex_);
          while (true) {
            condition_.wait(lock,
                            [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) break;
            auto task = std::move(tasks_.front());
            tasks_.pop_front();
            lock.unlock();
            task(*session_);
            lock.lock();
          }
          session_.reset();
        }) {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return ready_; });
    if (startup_error_) {
      lock.unlock();
      thread_.join();
      std::rethrow_exception(startup_error_);
    }
  }

  ~SessionExecutor() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_one();
    thread_.join();
  }

  SessionExecutor(const SessionExecutor&) = delete;
  SessionExecutor& operator=(const SessionExecutor&) = delete;

  template <typename Function>
  auto Invoke(Function&& function)
      -> std::invoke_result_t<Function, Session&> {
    using Result = std::invoke_result_t<Function, Session&>;
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        throw std::runtime_error("typed runtime is stopping");
      tasks_.emplace_back(
          [promise, function = std::forward<Function>(function)](
              Session& session) mutable {
            try {
              if constexpr (std::is_void_v<Result>) {
                std::invoke(function, session);
                promise->set_value();
              } else {
                promise->set_value(std::invoke(function, session));
              }
            } catch (...) {
              promise->set_exception(std::current_exception());
            }
          });
    }
    condition_.notify_one();
    return future.get();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::function<void(Session&)>> tasks_;
  std::unique_ptr<Session> session_;
  std::exception_ptr startup_error_;
  bool ready_{};
  bool stopping_{};
  std::thread thread_;
};

}  // namespace

struct RankingRuntime::Impl {
  Impl(const std::string& path, std::vector<std::string> providers)
      : executor([path, providers = std::move(providers)] {
          auto session = std::make_unique<RankingSession>(path, providers);
          session->SetCacheCapacity(
              CacheSetting("FOUNDRY_LOCAL_CLM_CACHE_CAPACITY", 256),
              CacheSetting("FOUNDRY_LOCAL_CLM_CACHE_CAPACITY_BYTES",
                           64 * 1024 * 1024));
          return session;
        }) {}
  SessionExecutor<RankingSession> executor;
};

RankingRuntime::RankingRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path, EnvironmentProviders())) {}
RankingRuntime::RankingRuntime(const std::string& package_path, std::string provider)
    : impl_(std::make_unique<Impl>(package_path, ExplicitProviders(provider))) {}
RankingRuntime::~RankingRuntime() = default;

nlohmann::json RankingRuntime::Rank(const NonGenerativeRequest& request) {
  auto translated = ToRequest(request);
  return impl_->executor.Invoke(
      [translated = std::move(translated)](RankingSession& session) mutable {
        return ToJson(session.Run(translated));
      });
}

nlohmann::json RankingRuntime::Rank(const RankingRequest& request) {
  OgaFreeFormRankRequest translated;
  translated.state = ToStructured(request.context);
  translated.instructions = request.question;
  translated.temperature = request.temperature;
  for (size_t i = 0; i < request.answers.size(); ++i)
    translated.candidates.emplace_back(std::to_string(i), request.answers[i]);
  const auto result = impl_->executor.Invoke(
      [translated = std::move(translated)](RankingSession& session) mutable {
        return session.Rank(translated);
      });
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
      : executor([path, providers = std::move(providers)] {
          auto session = std::make_unique<DecisionSession>(path, providers);
          session->SetCacheCapacity(
              CacheSetting("FOUNDRY_LOCAL_KEV_CACHE_CAPACITY", 512),
              CacheSetting("FOUNDRY_LOCAL_KEV_CACHE_CAPACITY_BYTES",
                           16 * 1024 * 1024));
          return session;
        }) {}
  SessionExecutor<DecisionSession> executor;
};

DecisionRuntime::DecisionRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path, EnvironmentProviders())) {}
DecisionRuntime::DecisionRuntime(const std::string& package_path, std::string provider)
    : impl_(std::make_unique<Impl>(package_path, ExplicitProviders(provider))) {}
DecisionRuntime::~DecisionRuntime() = default;

nlohmann::json DecisionRuntime::Decide(const NonGenerativeRequest& request) {
  auto translated = ToRequest(request);
  return impl_->executor.Invoke(
      [translated = std::move(translated)](DecisionSession& session) mutable {
        return ToJson(session.Decide(translated));
      });
}

}  // namespace fl
