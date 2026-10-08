// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace oatpp::web::server {
class HttpRequestHandler;
}

namespace fl {
class Model;
struct ServiceContext;

class NonGenerativeRuntimeState {
 public:
  NonGenerativeRuntimeState(uint64_t memory_budget_bytes,
                            size_t readiness_concurrency);
  std::shared_ptr<void> Acquire(
      const std::string& identity, const std::string& model_id, Model* owner,
      const std::function<std::shared_ptr<void>()>& factory);
  std::shared_ptr<void> Acquire(
      const std::string& identity, const std::string& model_id, Model* owner,
      uint64_t estimated_resident_bytes, const std::string& readiness_group,
      const std::function<std::shared_ptr<void>()>& factory);

 private:
  struct RuntimeEntry {
    std::string model_id;
    Model* owner;
    std::shared_future<std::shared_ptr<void>> runtime;
    std::shared_ptr<void> reservation;
    uint64_t resource_bytes{};
  };

  std::mutex mutex_;
  std::map<std::string, RuntimeEntry> runtimes_;
  uint64_t memory_budget_bytes_;
  uint64_t reserved_bytes_{};
  size_t readiness_concurrency_;
  std::mutex readiness_mutex_;
  std::condition_variable readiness_condition_;
  std::unordered_map<std::string, size_t> active_readiness_;

  friend bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
  friend void ClearNonGenerativeRuntimes(ServiceContext& ctx);
};

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    uint64_t memory_budget_bytes = std::numeric_limits<uint64_t>::max(),
    size_t readiness_concurrency = 1);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx);
bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
void ClearNonGenerativeRuntimes(ServiceContext& ctx);

}  // namespace fl
#include <condition_variable>
#include <cstdint>
