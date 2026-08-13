// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/genai_model_instance.h"

#include <memory>
#include <utility>

namespace fl {

/// Move-only lease on a loaded model, held for the whole lifetime of a SessionRuntime.
///
/// Two lifetime properties matter and neither is negotiable:
///
///  1. The lease holds a *strong* reference to the shared model entry, so it can outlive the
///     ModelLoadManager that handed it out without ever dereferencing a manager pointer. A lease carrying a
///     raw manager back-pointer could not make that claim.
///  2. The reference count, its mutex and the condition variable that signals release all live on the entry,
///     so the unload drain waits on storage the waiter itself holds a reference to.
///
/// Acquisition through ModelLoadManager::AcquireLoadedModel() performs the shutdown check, the loaded check
/// and the increment inside one manager critical section, closing the window where a model could be unloaded
/// between "it is loaded" and "I hold a reference to it".
class ModelSessionLease {
 public:
  ModelSessionLease() = default;

  /// Take a lease on an already-pinned instance. This compatibility path is for synchronized internal/test
  /// construction only: the caller must already exclude unload until this function has promoted the model's
  /// shared owner. Production creation uses ModelLoadManager::AcquireLoadedModel(), whose loaded-state check
  /// and lease increment are atomic against unload.
  static ModelSessionLease Adopt(GenAIModelInstance& model) {
    return ModelSessionLease(model.shared_from_this());
  }

  ~ModelSessionLease() noexcept {
    Release();
  }

  ModelSessionLease(ModelSessionLease&& other) noexcept : entry_(std::move(other.entry_)) {
    other.entry_.reset();
  }

  ModelSessionLease& operator=(ModelSessionLease&& other) noexcept {
    if (this != &other) {
      Release();
      entry_ = std::move(other.entry_);
      other.entry_.reset();
    }

    return *this;
  }

  ModelSessionLease(const ModelSessionLease&) = delete;
  ModelSessionLease& operator=(const ModelSessionLease&) = delete;

  /// True while this lease pins a model. A model-free runtime (unit tests) holds an empty lease.
  explicit operator bool() const noexcept { return entry_ != nullptr; }

  /// The leased model. Only valid while the lease is engaged.
  GenAIModelInstance& Model() const { return *entry_; }

 private:
  friend class ModelLoadManager;

  explicit ModelSessionLease(std::shared_ptr<GenAIModelInstance> entry) : entry_(std::move(entry)) {
    if (entry_) {
      entry_->AcquireSession();
    }
  }

  void Release() noexcept {
    if (entry_) {
      // Touches only the entry's own mutex and CV — never the manager, which may already be gone.
      entry_->ReleaseSession();
      entry_.reset();
    }
  }

  std::shared_ptr<GenAIModelInstance> entry_;
};

}  // namespace fl
