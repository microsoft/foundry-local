// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <future>
#include <functional>
#include <map>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fl {

class Model;

class NonGenerativeAdmissionCoordinator {
 public:
  NonGenerativeAdmissionCoordinator(uint64_t memory_budget_bytes,
                                    size_t readiness_concurrency);

 private:
  void Reserve(uint64_t bytes, const std::string& model_id);
  void Release(uint64_t bytes) noexcept;
  void BeginReadiness(const std::string& group);
  void EndReadiness(const std::string& group) noexcept;

  uint64_t memory_budget_bytes_;
  uint64_t reserved_bytes_{};
  size_t readiness_concurrency_;
  std::mutex mutex_;
  std::condition_variable readiness_condition_;
  std::unordered_map<std::string, size_t> active_readiness_;

  friend class NonGenerativeAdmissionReservation;
  friend class NonGenerativeReadinessLease;
};

class NonGenerativeAdmissionReservation {
 public:
  NonGenerativeAdmissionReservation(
      std::shared_ptr<NonGenerativeAdmissionCoordinator> coordinator,
      uint64_t bytes, const std::string& model_id);
  ~NonGenerativeAdmissionReservation();

  NonGenerativeAdmissionReservation(
      const NonGenerativeAdmissionReservation&) = delete;
  NonGenerativeAdmissionReservation& operator=(
      const NonGenerativeAdmissionReservation&) = delete;

 private:
  std::shared_ptr<NonGenerativeAdmissionCoordinator> coordinator_;
  uint64_t bytes_;
};

class NonGenerativeReadinessLease {
 public:
  NonGenerativeReadinessLease(
      std::shared_ptr<NonGenerativeAdmissionCoordinator> coordinator,
      std::string group);
  ~NonGenerativeReadinessLease();

  NonGenerativeReadinessLease(const NonGenerativeReadinessLease&) = delete;
  NonGenerativeReadinessLease& operator=(
      const NonGenerativeReadinessLease&) = delete;

 private:
  std::shared_ptr<NonGenerativeAdmissionCoordinator> coordinator_;
  std::string group_;
};

class NonGenerativeRuntimeState {
 public:
  explicit NonGenerativeRuntimeState(
      std::shared_ptr<NonGenerativeAdmissionCoordinator> admission);
  std::shared_ptr<void> Acquire(
      const std::string& identity, const std::string& model_id, Model* owner,
      const std::function<std::shared_ptr<void>()>& factory) {
    return Acquire(identity, model_id, owner, 0, "", factory);
  }
  std::shared_ptr<void> Acquire(
      const std::string& identity, const std::string& model_id, Model* owner,
      uint64_t estimated_resident_bytes, const std::string& readiness_group,
      const std::function<std::shared_ptr<void>()>& factory);
  bool Remove(Model& model);
  void Clear();
  void RejectNewAcquisitions();

 private:
  struct ManagedRuntime {
    std::shared_ptr<NonGenerativeAdmissionReservation> reservation;
    std::shared_ptr<void> runtime;
    Model* owner{};
    ~ManagedRuntime();
  };

  struct RuntimeEntry {
    std::string model_id;
    Model* owner;
    std::shared_future<std::shared_ptr<ManagedRuntime>> runtime;
  };

  std::mutex mutex_;
  std::map<std::string, RuntimeEntry> runtimes_;
  std::shared_ptr<NonGenerativeAdmissionCoordinator> admission_;
  bool shutting_down_{};
};

std::shared_ptr<NonGenerativeAdmissionCoordinator>
CreateNonGenerativeAdmissionCoordinator(
    uint64_t memory_budget_bytes, size_t readiness_concurrency);
std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> admission);
std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    uint64_t memory_budget_bytes =
        (std::numeric_limits<uint64_t>::max)(),
    size_t readiness_concurrency = 1);

}  // namespace fl
