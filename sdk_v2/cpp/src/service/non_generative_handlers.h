// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/predictive/non_generative_admission.h"

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
  explicit NonGenerativeRuntimeState(
      std::shared_ptr<NonGenerativeAdmissionCoordinator> admission);
  ~NonGenerativeRuntimeState();
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
    std::shared_ptr<NonGenerativeAdmissionReservation> reservation;
    std::shared_future<std::shared_ptr<void>> runtime;
  };

  std::mutex mutex_;
  std::map<std::string, RuntimeEntry> runtimes_;
  std::shared_ptr<NonGenerativeAdmissionCoordinator> admission_;

  friend bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
  friend void ClearNonGenerativeRuntimes(ServiceContext& ctx);
};

std::shared_ptr<NonGenerativeAdmissionCoordinator>
CreateNonGenerativeAdmissionCoordinator(
    uint64_t memory_budget_bytes = (std::numeric_limits<uint64_t>::max)(),
    size_t readiness_concurrency = 1);
std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> admission);
std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    uint64_t memory_budget_bytes = (std::numeric_limits<uint64_t>::max)(),
    size_t readiness_concurrency = 1);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx);
bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
void ClearNonGenerativeRuntimes(ServiceContext& ctx);

}  // namespace fl
#include <cstdint>
