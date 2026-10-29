// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_admission.h"

#include <fmt/format.h>

#include <stdexcept>
#include <utility>

namespace fl {

NonGenerativeAdmissionCoordinator::NonGenerativeAdmissionCoordinator(
    uint64_t memory_budget_bytes, size_t readiness_concurrency)
    : memory_budget_bytes_(memory_budget_bytes),
      readiness_concurrency_(readiness_concurrency) {
  if (!memory_budget_bytes_)
    throw std::invalid_argument(
        "non-generative memory budget must be positive");
  if (!readiness_concurrency_)
    throw std::invalid_argument(
        "non-generative readiness concurrency must be positive");
}

void NonGenerativeAdmissionCoordinator::Reserve(
    uint64_t bytes, const std::string& model_id) {
  std::lock_guard lock(mutex_);
  if (bytes > memory_budget_bytes_ - reserved_bytes_)
    throw std::runtime_error(fmt::format(
        "loading '{}' requires {} bytes but only {} bytes remain in the "
        "non-generative runtime budget",
        model_id, bytes, memory_budget_bytes_ - reserved_bytes_));
  reserved_bytes_ += bytes;
}

void NonGenerativeAdmissionCoordinator::Release(uint64_t bytes) noexcept {
  std::lock_guard lock(mutex_);
  reserved_bytes_ -= bytes;
}

void NonGenerativeAdmissionCoordinator::BeginReadiness(
    const std::string& group) {
  std::unique_lock lock(mutex_);
  readiness_condition_.wait(lock, [&] {
    return active_readiness_[group] < readiness_concurrency_;
  });
  ++active_readiness_[group];
}

void NonGenerativeAdmissionCoordinator::EndReadiness(
    const std::string& group) noexcept {
  {
    std::lock_guard lock(mutex_);
    const auto found = active_readiness_.find(group);
    if (found != active_readiness_.end() && --found->second == 0)
      active_readiness_.erase(found);
  }
  readiness_condition_.notify_all();
}

NonGenerativeAdmissionReservation::NonGenerativeAdmissionReservation(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> coordinator,
    uint64_t bytes, const std::string& model_id)
    : coordinator_(std::move(coordinator)), bytes_(bytes) {
  coordinator_->Reserve(bytes_, model_id);
}

NonGenerativeAdmissionReservation::~NonGenerativeAdmissionReservation() {
  coordinator_->Release(bytes_);
}

NonGenerativeReadinessLease::NonGenerativeReadinessLease(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> coordinator,
    std::string group)
    : coordinator_(std::move(coordinator)), group_(std::move(group)) {
  coordinator_->BeginReadiness(group_);
}

NonGenerativeReadinessLease::~NonGenerativeReadinessLease() {
  coordinator_->EndReadiness(group_);
}

}  // namespace fl
