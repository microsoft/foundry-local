// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fl {

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

}  // namespace fl
