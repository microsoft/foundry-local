// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_runtime.h"
#include "catalog/non_generative_package.h"
#include "utils.h"

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

std::unique_ptr<::NonGenerativeSession> CreateSession(
    const std::string& path, const std::vector<std::string>& providers) {
  auto config = OgaConfig::Create(path.c_str());
  if (!providers.empty()) {
    config->ClearProviders();
    for (const auto& provider : providers)
      config->AppendProvider(provider.c_str());
  }
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
          return CreateSession(path, providers);
        }) {}
  SessionExecutor<::NonGenerativeSession> executor;
};

RankingRuntime::RankingRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path, EnvironmentProviders())) {}
RankingRuntime::RankingRuntime(const std::string& package_path, std::string provider)
    : impl_(std::make_unique<Impl>(package_path, ExplicitProviders(provider))) {}
RankingRuntime::~RankingRuntime() = default;

nlohmann::json RankingRuntime::Rank(const RankingRequest& request) {
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
      });
  const auto result = json::parse(response);
  json ranked = json::array();
  for (const auto& item : result.at("ranked")) {
    const auto key = item.at("key").get<std::string>();
    ranked.push_back({
        {"rank", item.at("rank")},
        {"candidate", request.answers.at(static_cast<size_t>(std::stoull(key)))},
        {"prob", item.at("probability")},
    });
  }
  return {{"model", result.at("model")}, {"ranked", std::move(ranked)}};
}

struct DecisionRuntime::Impl {
  Impl(const std::string& path, std::vector<std::string> providers)
      : executor([path, providers = std::move(providers)] {
          return CreateSession(path, providers);
        }) {}
  SessionExecutor<::NonGenerativeSession> executor;
};

DecisionRuntime::DecisionRuntime(const std::string& package_path)
    : impl_(std::make_unique<Impl>(package_path, EnvironmentProviders())) {}
DecisionRuntime::DecisionRuntime(const std::string& package_path, std::string provider)
    : impl_(std::make_unique<Impl>(package_path, ExplicitProviders(provider))) {}
DecisionRuntime::~DecisionRuntime() = default;

nlohmann::json DecisionRuntime::Decide(const NonGenerativeRequest& request) {
  const auto request_json = StructuredRequestJson(request).dump();
  const auto response = impl_->executor.Invoke(
      [request_json](::NonGenerativeSession& session) {
        return session.Run(request_json);
      });
  return json::parse(response).at("answers");
}

}  // namespace fl
