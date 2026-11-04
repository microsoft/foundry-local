// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/predictive/non_generative_admission.h"
#include "model.h"

#include <fmt/format.h>

#include <stdexcept>
#include <utility>

namespace fl {

class ManagedModelRuntime {
 public:
  virtual ~ManagedModelRuntime() = default;
  virtual std::shared_ptr<void> Lease() = 0;
  virtual Model* Owner() const noexcept = 0;
};

namespace {

class ManagedTypedRuntime final
    : public ManagedModelRuntime,
      public std::enable_shared_from_this<ManagedTypedRuntime> {
 public:
  ManagedTypedRuntime(
      std::shared_ptr<NonGenerativeAdmissionReservation> reservation,
      std::shared_ptr<void> runtime, Model* owner)
      : reservation_(std::move(reservation)),
        runtime_(std::move(runtime)),
        owner_(owner) {}

  ~ManagedTypedRuntime() override {
    runtime_.reset();
    if (owner_) owner_->ReleaseExternalRuntime();
    reservation_.reset();
  }

  std::shared_ptr<void> Lease() override {
    return std::shared_ptr<void>(shared_from_this(), runtime_.get());
  }

  Model* Owner() const noexcept override { return owner_; }

 private:
  std::shared_ptr<NonGenerativeAdmissionReservation> reservation_;
  std::shared_ptr<void> runtime_;
  Model* owner_;
};

}  // namespace

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

NonGenerativeRuntimeState::NonGenerativeRuntimeState(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> admission)
    : admission_(std::move(admission)) {
  if (!admission_)
    throw std::invalid_argument(
        "non-generative admission coordinator is required");
}

std::shared_ptr<void> NonGenerativeRuntimeState::Acquire(
    const std::string& identity, const std::string& model_id, Model* owner,
    uint64_t estimated_resident_bytes, const std::string& readiness_group,
    const std::function<std::shared_ptr<void>()>& factory) {
  std::shared_future<std::shared_ptr<ManagedModelRuntime>> future;
  std::shared_ptr<std::promise<std::shared_ptr<ManagedModelRuntime>>> promise;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_)
      throw std::runtime_error(
          "cannot acquire non-generative runtime during shutdown");
    const auto found = runtimes_.find(identity);
    if (found != runtimes_.end()) {
      future = found->second.runtime;
    } else {
      promise =
          std::make_shared<
              std::promise<std::shared_ptr<ManagedModelRuntime>>>();
      future = promise->get_future().share();
      runtimes_.emplace(identity,
                        RuntimeEntry{model_id, owner, future});
    }
  }

  if (promise) {
    try {
      auto reservation =
          std::make_shared<NonGenerativeAdmissionReservation>(
              admission_, estimated_resident_bytes, model_id);
      NonGenerativeReadinessLease readiness(admission_, readiness_group);
      auto managed = std::make_shared<ManagedTypedRuntime>(
          std::move(reservation), factory(), owner);
      promise->set_value(managed);
    } catch (...) {
      promise->set_exception(std::current_exception());
      std::lock_guard lock(mutex_);
      runtimes_.erase(identity);
      throw;
    }
  }

  auto managed = future.get();
  return managed->Lease();
}

bool NonGenerativeRuntimeState::Remove(Model& model) {
  std::vector<RuntimeEntry> removed;
  auto* leaf = model.SelectedLeaf();
  {
    std::lock_guard lock(mutex_);
    for (auto it = runtimes_.begin(); it != runtimes_.end();) {
      if (it->second.owner == leaf) {
        removed.push_back(std::move(it->second));
        it = runtimes_.erase(it);
      } else {
        ++it;
      }
    }
  }
  const bool did_remove = !removed.empty();
  removed.clear();
  return did_remove;
}

void NonGenerativeRuntimeState::Clear() {
  std::map<std::string, RuntimeEntry> removed;
  {
    std::lock_guard lock(mutex_);
    removed.swap(runtimes_);
  }
  removed.clear();
}

void NonGenerativeRuntimeState::RejectNewAcquisitions() {
  std::lock_guard lock(mutex_);
  shutting_down_ = true;
}

std::shared_ptr<NonGenerativeAdmissionCoordinator>
CreateNonGenerativeAdmissionCoordinator(
    uint64_t memory_budget_bytes, size_t readiness_concurrency) {
  return std::make_shared<NonGenerativeAdmissionCoordinator>(
      memory_budget_bytes, readiness_concurrency);
}

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    std::shared_ptr<NonGenerativeAdmissionCoordinator> admission) {
  return std::make_shared<NonGenerativeRuntimeState>(std::move(admission));
}

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState(
    uint64_t memory_budget_bytes, size_t readiness_concurrency) {
  return CreateNonGenerativeRuntimeState(
      CreateNonGenerativeAdmissionCoordinator(
          memory_budget_bytes, readiness_concurrency));
}

}  // namespace fl
